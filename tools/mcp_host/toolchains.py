from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
import tempfile
from typing import Any, Iterable

from .model import ToolError
from .tasks import TaskContext
from .workspace import Workspace, sha256_file


DIAGNOSTIC_PATTERNS = [
    re.compile(r"^(?P<file>.+?)\((?P<line>\d+)(?:,(?P<column>\d+))?\):\s*(?P<level>fatal error|error|warning)\s*(?P<code>[A-Za-z]+\d+)?:?\s*(?P<message>.*)$"),
    re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<column>\d+):\s*(?P<level>fatal error|error|warning):\s*(?P<message>.*)$"),
]


@dataclass(slots=True)
class CommandResult:
    command: list[str]
    cwd: str
    return_code: int
    output: list[str]
    diagnostics: list[dict[str, Any]]
    cancelled: bool

    def as_dict(self) -> dict[str, Any]:
        return {
            "command": self.command,
            "cwd": self.cwd,
            "returnCode": self.return_code,
            "output": self.output,
            "diagnostics": self.diagnostics,
            "cancelled": self.cancelled,
        }


class CommandRunner:
    def __init__(self, output_limit: int = 10000) -> None:
        self.output_limit = max(100, output_limit)

    def run(
        self,
        context: TaskContext,
        command: Iterable[str],
        *,
        cwd: Path,
        environment: dict[str, str] | None = None,
        timeout_seconds: float | None = None,
    ) -> CommandResult:
        argv = [str(argument) for argument in command]
        context.progress("launching process", 0.05)
        try:
            process = subprocess.Popen(
                argv,
                cwd=cwd,
                env={**os.environ, **(environment or {})},
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
                start_new_session=os.name != "nt",
            )
        except FileNotFoundError as error:
            raise ToolError(
                "toolchain.executable_not_found",
                f"Executable '{argv[0]}' was not found",
                stage="toolchain.launch",
                context={"command": argv, "cwd": cwd.as_posix()},
                suggested_actions=["Configure the required tool executable or install the missing toolchain"],
            ) from error
        except OSError as error:
            raise ToolError("toolchain.launch_failed", str(error), stage="toolchain.launch", context={"command": argv, "cwd": cwd.as_posix()}) from error

        context.attach_process(process)
        output: list[str] = []
        diagnostics: list[dict[str, Any]] = []
        timer: threading.Timer | None = None
        timed_out = threading.Event()
        if timeout_seconds:
            def terminate_for_timeout() -> None:
                timed_out.set()
                if process.poll() is None:
                    TaskContext.terminate_process(process)
            timer = threading.Timer(timeout_seconds, terminate_for_timeout)
            timer.start()
        try:
            assert process.stdout is not None
            for line in process.stdout:
                context.check_cancelled("toolchain.execute")
                cleaned = line.rstrip("\r\n")
                context.output(cleaned)
                output.append(cleaned)
                if len(output) > self.output_limit:
                    del output[: len(output) - self.output_limit]
                diagnostic = self._parse_diagnostic(cleaned)
                if diagnostic:
                    diagnostics.append(diagnostic)
            return_code = process.wait()
        finally:
            if timer:
                timer.cancel()
            context.detach_process()

        if context.cancelled.is_set():
            raise ToolError("operation.cancelled", "Process task was cancelled", stage="toolchain.execute", status="cancelled")
        if timed_out.is_set():
            raise ToolError(
                "operation.timeout",
                "Process exceeded its configured timeout",
                stage="toolchain.execute",
                context={"command": argv, "timeoutSeconds": timeout_seconds, "returnCode": return_code},
                retryable=True,
            )
        return CommandResult(argv, cwd.as_posix(), return_code, output, diagnostics, False)

    @staticmethod
    def _parse_diagnostic(line: str) -> dict[str, Any] | None:
        for pattern in DIAGNOSTIC_PATTERNS:
            match = pattern.match(line)
            if match:
                values = match.groupdict()
                return {
                    "file": values.get("file"),
                    "line": int(values["line"]),
                    "column": int(values["column"]) if values.get("column") else None,
                    "level": values["level"],
                    "code": values.get("code"),
                    "message": values["message"],
                }
        return None


class Toolchains:
    def __init__(self, workspace: Workspace, configured: dict[str, str] | None = None) -> None:
        self.workspace = workspace
        self.configured = configured or {}
        self.runner = CommandRunner()

    def executable(self, name: str, candidates: list[str]) -> str | None:
        configured = self.configured.get(name)
        if configured:
            path = Path(configured).expanduser()
            if path.is_file():
                return str(path.resolve())
            resolved = shutil.which(configured)
            if resolved:
                return resolved
        for candidate in candidates:
            resolved = shutil.which(candidate)
            if resolved:
                return resolved
        return None

    def snapshot(self) -> dict[str, Any]:
        probes = {
            "cmake": self.executable("cmake", ["cmake"]),
            "ninja": self.executable("ninja", ["ninja"]),
            "python": self.executable("python", ["python", "python3"]),
            "unrealEditor": self._editor_executable(),
            "unrealPak": self.executable("unrealPak", ["UnrealPak.exe", "UnrealPak"]),
            "runUAT": self.executable("runUAT", ["RunUAT.bat", "RunUAT.sh"]),
            "debugger": self.executable("debugger", ["lldb-dap", "lldb-vscode"]),
        }
        return {name: {"available": path is not None, "path": path} for name, path in probes.items()}
    def editor_profile(
        self,
        *,
        root: str | None = None,
        project_path: str | None = None,
        target_version: str | None = None,
    ) -> dict[str, Any]:
        tools = self.snapshot()
        editor_path = tools["unrealEditor"]["path"]
        evidence: list[dict[str, Any]] = []
        missing_conditions: list[str] = []
        project_version: str | None = None
        if project_path is not None:
            if root is None:
                raise ToolError(
                    "transport.invalid_arguments",
                    "root is required when projectPath is supplied",
                    stage="transport.arguments",
                )
            project = self.workspace.resolve(root, project_path, must_exist=True)
            if not project.path.is_file() or project.path.suffix.lower() != ".uproject":
                raise ToolError(
                    "editor.project_invalid",
                    "projectPath must identify a .uproject file",
                    stage="editor.profile",
                    context={"root": root, "projectPath": project.relative},
                )
            try:
                descriptor = json.loads(project.path.read_text(encoding="utf-8-sig"))
            except (OSError, UnicodeError, json.JSONDecodeError) as error:
                raise ToolError(
                    "editor.project_invalid",
                    "The .uproject descriptor could not be read as JSON",
                    stage="editor.profile",
                    context={"root": root, "projectPath": project.relative, "error": str(error)},
                ) from error
            association = descriptor.get("EngineAssociation")
            if isinstance(association, str) and association.strip():
                project_version = self._normalized_unreal_version(association)
                evidence.append(
                    {
                        "kind": "project_descriptor",
                        "source": project.relative,
                        "engineAssociation": association,
                        "normalizedVersion": project_version,
                    }
                )
            else:
                missing_conditions.append("The supplied .uproject does not declare EngineAssociation")

        requested_version = self._normalized_unreal_version(target_version) if target_version else None
        if target_version:
            evidence.append(
                {
                    "kind": "target_version",
                    "source": "request",
                    "targetVersion": target_version,
                    "normalizedVersion": requested_version,
                }
            )
            if requested_version is None:
                missing_conditions.append("targetVersion does not contain a recognizable Unreal major.minor version")
        if requested_version and project_version and requested_version != project_version:
            missing_conditions.append("targetVersion does not match the supplied project's EngineAssociation")

        configured_version_value = self.configured.get("unrealEditorVersion")
        editor_version = self._normalized_unreal_version(configured_version_value) if configured_version_value else self._normalized_unreal_version(editor_path)
        if editor_path:
            evidence.append(
                {
                    "kind": "editor_executable",
                    "source": editor_path,
                    "configured": bool(self.configured.get("unrealEditor")),
                    "declaredVersion": configured_version_value,
                    "normalizedVersion": editor_version,
                }
            )
        target_evidence_version = requested_version or project_version
        version_evidence_consistent = not (requested_version and project_version and requested_version != project_version)
        verified = bool(editor_path and editor_version and target_evidence_version and editor_version == target_evidence_version and version_evidence_consistent)
        version_mismatch = bool(editor_path and editor_version and target_evidence_version and editor_version != target_evidence_version)
        configured = editor_path is not None
        unverified = configured and not verified and not version_mismatch
        degraded = configured and not verified

        if not editor_path:
            missing_conditions.append(
                "Configure tools.unrealEditor with UnrealEditor-Cmd.exe or UE4Editor-Cmd.exe matching the target game engine version"
            )
        elif editor_version is None:
            missing_conditions.append(
                "Configure tools.unrealEditorVersion or use an editor path containing an Unreal major.minor version"
            )
        if target_evidence_version is None:
            missing_conditions.append("Supply projectPath with EngineAssociation or an explicit targetVersion")
        if version_mismatch:
            missing_conditions.append("The configured Unreal Editor version does not match the project or target version")

        supported_operations: list[str] = []
        degraded_operations: list[str] = []
        if verified:
            supported_operations.extend(["runScript", "cook"])
        elif configured:
            degraded_operations.extend(["runScript", "cook"])
        if tools["runUAT"]["available"]:
            if verified:
                supported_operations.append("package")
            else:
                degraded_operations.append("package")
        else:
            missing_conditions.append("Configure tools.runUAT with RunUAT.bat or RunUAT.sh for BuildCookRun or BuildPlugin")
        if tools["unrealPak"]["available"]:
            supported_operations.append("unrealPak")
        else:
            missing_conditions.append("Configure tools.unrealPak with UnrealPak.exe for direct package assembly")

        compatibility = "verified" if verified else "degraded" if degraded else "unconfigured"
        return {
            "available": configured,
            "configured": configured,
            "compatible": True if verified else False if version_mismatch else None,
            "compatibility": compatibility,
            "degraded": degraded,
            "unverified": unverified,
            "path": editor_path,
            "editorVersion": editor_version,
            "targetVersion": target_evidence_version,
            "projectVersion": project_version,
            "runUAT": tools["runUAT"],
            "unrealPak": tools["unrealPak"],
            "supportedOperations": supported_operations,
            "degradedOperations": degraded_operations,
            "missingConditions": missing_conditions,
            "evidence": evidence,
        }

    def cmake_build_task(
        self,
        context: TaskContext,
        *,
        root: str,
        source_path: str,
        build_path: str,
        target: str | None,
        configure_args: list[str],
        build_args: list[str],
        install_path: str | None,
        timeout_seconds: float,
    ) -> dict[str, Any]:
        cmake = self.executable("cmake", ["cmake"])
        if not cmake:
            raise ToolError("toolchain.cmake_unavailable", "CMake was not found", stage="cpp.configure", suggested_actions=["Configure --tool cmake=<path>"])
        source = self.workspace.resolve(root, source_path, must_exist=True)
        build = self.workspace.resolve(root, build_path)
        build.path.mkdir(parents=True, exist_ok=True)

        configure = self.runner.run(context, [cmake, "-S", str(source.path), "-B", str(build.path), *configure_args], cwd=source.root, timeout_seconds=timeout_seconds)
        if configure.return_code != 0:
            raise ToolError(
                "mod.build_failed",
                "CMake configure failed",
                stage="cpp.configure",
                context={"process": configure.as_dict()},
                suggested_actions=["Fix the reported configure diagnostics and submit another build"],
            )
        context.progress("building", 0.45)
        command = [cmake, "--build", str(build.path)]
        if target:
            command.extend(["--target", target])
        command.extend(build_args)
        built = self.runner.run(context, command, cwd=source.root, timeout_seconds=timeout_seconds)
        if built.return_code != 0:
            raise ToolError(
                "mod.build_failed",
                "C++ build failed",
                stage="cpp.build",
                context={"process": built.as_dict()},
                suggested_actions=["Fix the reported compiler diagnostics and submit another build"],
            )

        artifacts: list[dict[str, Any]] = []
        artifacts_truncated = False
        changes: list[dict[str, Any]] = []
        if install_path:
            context.progress("installing", 0.85)
            install = self.workspace.resolve(root, install_path)
            install.path.mkdir(parents=True, exist_ok=True)
            install_command = [cmake, "--install", str(build.path), "--prefix", str(install.path)]
            if "--config" in build_args:
                config_index = build_args.index("--config")
                if config_index + 1 < len(build_args):
                    install_command.extend(["--config", build_args[config_index + 1]])
            installed = self.runner.run(context, install_command, cwd=source.root, timeout_seconds=timeout_seconds)
            if installed.return_code != 0:
                raise ToolError("mod.install_failed", "CMake install failed", stage="cpp.install", context={"process": installed.as_dict()})
            changes.append({"kind": "directory", "action": "installed", "path": install.relative})
            installed_manifest = self.workspace.manifest(root, install.relative, recursive=True, limit=1000)
            artifacts = installed_manifest["artifacts"]
            artifacts_truncated = installed_manifest["truncated"]
        else:
            artifact_extensions = {".dll", ".exe", ".lib", ".pdb", ".exp", ".pak", ".utoc", ".ucas", ".zip"}
            ignored_directories = {"_deps", "CMakeFiles", "RE-UE4SS", "corrosion", "x64"}
            artifact_paths = sorted(
                (
                    path
                    for path in build.path.rglob("*")
                    if path.is_file()
                    and path.suffix.lower() in artifact_extensions
                    and not ignored_directories.intersection(path.relative_to(build.path).parts)
                ),
                key=lambda path: path.stat().st_mtime_ns,
                reverse=True,
            )
            artifacts = [
                {
                    "path": path.relative_to(build.root).as_posix(),
                    "size": path.stat().st_size,
                    "sha256": sha256_file(path),
                    "modifiedNs": path.stat().st_mtime_ns,
                }
                for path in artifact_paths[:100]
            ]
            artifacts_truncated = len(artifact_paths) > len(artifacts)
        return {
            "currentState": {
                "configured": True,
                "built": True,
                "target": target,
                "buildDirectory": build.relative,
                "artifacts": artifacts,
                "artifactsTruncated": artifacts_truncated,
            },
            "actualChanges": changes,
            "evidence": [
                {"kind": "process", "source": "cmake.configure", **configure.as_dict()},
                {"kind": "process", "source": "cmake.build", **built.as_dict()},
            ],
            "restartRequired": bool(install_path),
        }

    def run_editor_script_task(
        self,
        context: TaskContext,
        *,
        root: str,
        project_path: str,
        script_path: str,
        arguments: list[str],
        timeout_seconds: float,
    ) -> dict[str, Any]:
        editor = self._editor_executable()
        if not editor:
            raise ToolError(
                "editor.profile_unavailable",
                "A matching UnrealEditor-Cmd executable is not configured",
                stage="editor.profile",
                context={"configured": self.configured.get("unrealEditor")},
                suggested_actions=["Configure --tool unrealEditor=<UnrealEditor-Cmd.exe> for the target engine version"],
            )
        project = self.workspace.resolve(root, project_path, must_exist=True)
        script = self.workspace.resolve(root, script_path, must_exist=True)
        context.progress("running Unreal Editor commandlet", 0.1)
        command = [editor, str(project.path), f"-ExecutePythonScript={script.path}", "-unattended", "-nop4", "-nosplash", *arguments]
        result = self.runner.run(context, command, cwd=project.root, timeout_seconds=timeout_seconds)
        if result.return_code != 0:
            raise ToolError(
                "editor.command_failed",
                "Unreal Editor commandlet failed",
                stage="editor.execute",
                context={"process": result.as_dict()},
                suggested_actions=["Inspect the editor diagnostics and confirm the project/editor version match"],
            )
        return {
            "currentState": {"completed": True, "project": project.relative, "script": script.relative},
            "evidence": [{"kind": "process", "source": "UnrealEditor-Cmd", **result.as_dict()}],
        }

    def editor_cook_task(
        self,
        context: TaskContext,
        *,
        root: str,
        project_path: str,
        target_platform: str,
        maps: list[str],
        cook_directories: list[str],
        output_path: str | None,
        arguments: list[str],
        timeout_seconds: float,
    ) -> dict[str, Any]:
        editor = self._editor_executable()
        if not editor:
            raise ToolError("editor.profile_unavailable", "A matching UnrealEditor-Cmd executable is not configured", stage="editor.profile")
        project = self.workspace.resolve(root, project_path, must_exist=True)
        command = [editor, str(project.path), "-run=Cook", f"-TargetPlatform={target_platform}", "-unattended", "-nop4", "-nosplash"]
        command.extend(f"-Map={map_name}" for map_name in maps)
        command.extend(f"-CookDir={directory}" for directory in cook_directories)
        output = self.workspace.resolve(root, output_path) if output_path else None
        if output:
            output.path.mkdir(parents=True, exist_ok=True)
            command.append(f"-OutputDir={output.path}")
        command.extend(arguments)
        result = self.runner.run(context, command, cwd=project.root, timeout_seconds=timeout_seconds)
        if result.return_code != 0:
            raise ToolError("editor.cook_failed", "Unreal Editor cook failed", stage="editor.cook", context={"process": result.as_dict()})
        artifacts = self.workspace.manifest(root, output.relative, recursive=True, limit=10000) if output else None
        return {
            "currentState": {"cooked": True, "project": project.relative, "targetPlatform": target_platform, "artifacts": artifacts["artifacts"] if artifacts else []},
            "actualChanges": [{"kind": "cook_output", "action": "created", "path": output.relative}] if output else [],
            "evidence": [{"kind": "process", "source": "UnrealEditor-Cmd Cook", **result.as_dict()}],
        }

    def run_uat_task(
        self,
        context: TaskContext,
        *,
        root: str,
        project_path: str,
        command_name: str,
        arguments: list[str],
        artifact_path: str | None,
        timeout_seconds: float,
    ) -> dict[str, Any]:
        run_uat = self.executable("runUAT", ["RunUAT.bat", "RunUAT.sh"])
        if not run_uat:
            raise ToolError("toolchain.uat_unavailable", "RunUAT is not configured", stage="editor.package")
        project = self.workspace.resolve(root, project_path, must_exist=True)
        resolved_artifact_path = artifact_path.strip() if artifact_path and artifact_path.strip() else None
        artifact_path_source = "artifactPath"
        if resolved_artifact_path is None:
            resolved_artifact_path = self._uat_artifact_path(command_name, arguments)
            artifact_path_source = "arguments"
        if resolved_artifact_path is None:
            raise ToolError(
                "editor.package_artifact_path_required",
                "artifactPath is required unless a package output path can be derived from UAT arguments",
                stage="editor.package",
                context={"command": command_name, "recognizedArguments": ["-Package=", "-ArchiveDirectory=", "-StagingDirectory="]},
                suggested_actions=["Supply artifactPath or an explicit UAT package/archive/staging output argument"],
                status="rejected",
            )
        result = self.runner.run(
            context,
            [run_uat, command_name, f"-project={project.path}", *arguments],
            cwd=project.root,
            timeout_seconds=timeout_seconds,
        )
        if result.return_code != 0:
            raise ToolError("editor.package_failed", "Unreal Automation Tool failed", stage="editor.package", context={"process": result.as_dict()})
        try:
            manifest = self.workspace.manifest(root, resolved_artifact_path, recursive=True, limit=10000)
        except ToolError as error:
            if error.code != "artifact.not_found":
                raise
            raise ToolError(
                "editor.package_missing_artifacts",
                "Unreal Automation Tool reported success but the package artifact path does not exist",
                stage="editor.package.verify",
                context={"artifactPath": resolved_artifact_path, "artifactPathSource": artifact_path_source, "process": result.as_dict()},
                suggested_actions=["Verify the UAT output argument and supply the exact artifactPath"],
            ) from error
        artifacts = manifest["artifacts"]
        if not artifacts:
            raise ToolError(
                "editor.package_missing_artifacts",
                "Unreal Automation Tool reported success but produced no files in the package artifact path",
                stage="editor.package.verify",
                context={"artifactPath": manifest["path"], "artifactPathSource": artifact_path_source, "process": result.as_dict()},
                suggested_actions=["Inspect the UAT output and point artifactPath at the packaged files"],
            )
        return {
            "currentState": {
                "completed": True,
                "command": command_name,
                "project": project.relative,
                "artifactPath": manifest["path"],
                "artifactPathSource": artifact_path_source,
                "artifacts": artifacts,
                "artifactsTruncated": manifest["truncated"],
            },
            "actualChanges": [
                {
                    "kind": "package_output",
                    "action": "verified",
                    "path": manifest["path"],
                    "artifactCount": len(artifacts),
                    "artifactsTruncated": manifest["truncated"],
                }
            ],
            "evidence": [
                {"kind": "process", "source": "RunUAT", **result.as_dict()},
                {
                    "kind": "artifact_manifest",
                    "source": manifest["path"],
                    "artifacts": artifacts,
                    "artifactsTruncated": manifest["truncated"],
                },
            ],
        }

    def unrealpak_task(
        self,
        context: TaskContext,
        *,
        root: str,
        output_path: str,
        entries: list[dict[str, str]],
        arguments: list[str],
        timeout_seconds: float,
    ) -> dict[str, Any]:
        unreal_pak = self.executable("unrealPak", ["UnrealPak.exe", "UnrealPak"])
        if not unreal_pak:
            raise ToolError("toolchain.unrealpak_unavailable", "UnrealPak is not configured", stage="blueprint.package")
        output = self.workspace.resolve(root, output_path)
        output.path.parent.mkdir(parents=True, exist_ok=True)
        before_hash = sha256_file(output.path) if output.path.is_file() else None
        response_lines = []
        for entry in entries:
            source = self.workspace.resolve(root, entry["sourcePath"], must_exist=True)
            mount_path = entry["mountPath"]
            if not source.path.is_file():
                raise ToolError("artifact.not_file", "Every UnrealPak source must be a file", stage="blueprint.package", context={"path": source.relative})
            if any(character in mount_path for character in {'"', "\r", "\n"}) or not mount_path:
                raise ToolError("artifact.invalid_mount_path", "UnrealPak mountPath is empty or contains unsupported characters", stage="blueprint.package", context={"mountPath": mount_path})
            response_lines.append(f'\"{source.path}\" \"{mount_path}\"')
        if not response_lines:
            raise ToolError("artifact.empty_package", "At least one package entry is required", stage="blueprint.package")
        response_file: Path | None = None
        try:
            with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", prefix=".ue4ss-unrealpak-", dir=output.path.parent, delete=False) as file:
                file.write("\n".join(response_lines) + "\n")
                response_file = Path(file.name)
            result = self.runner.run(
                context,
                [unreal_pak, str(output.path), f"-Create={response_file}", *arguments],
                cwd=output.root,
                timeout_seconds=timeout_seconds,
            )
        finally:
            if response_file:
                response_file.unlink(missing_ok=True)
        if result.return_code != 0:
            raise ToolError("blueprint.package_failed", "UnrealPak failed", stage="blueprint.package", context={"process": result.as_dict()})
        if not output.path.is_file():
            raise ToolError("blueprint.package_missing", "UnrealPak reported success but did not create the requested package", stage="blueprint.package")
        artifacts = []
        for artifact in sorted(output.path.parent.glob(f"{output.path.stem}.*")):
            if artifact.is_file():
                artifacts.append({"path": artifact.relative_to(output.root).as_posix(), "size": artifact.stat().st_size, "sha256": sha256_file(artifact)})
        after_hash = sha256_file(output.path)
        return {
            "currentState": {"packaged": True, "output": output.relative, "artifacts": artifacts},
            "actualChanges": [{"kind": "package", "action": "overwritten" if before_hash else "created", "path": output.relative, "beforeSha256": before_hash, "afterSha256": after_hash}],
            "evidence": [{"kind": "process", "source": "UnrealPak", **result.as_dict()}, {"kind": "artifact_manifest", "source": output.path.as_posix(), "artifacts": artifacts}],
        }

    def crash_artifacts(self, roots: list[tuple[str, str]]) -> dict[str, Any]:
        artifacts = []
        seen: set[tuple[str, str]] = set()
        truncated = False
        for root_name, relative in roots:
            try:
                scoped = self.workspace.resolve(root_name, relative, must_exist=True)
            except ToolError:
                continue
            paths = [scoped.path] if scoped.path.is_file() else scoped.path.rglob("*")
            for path in paths:
                if not path.is_file() or path.suffix.lower() not in {".dmp", ".mdmp", ".log", ".crash", ".xml"}:
                    continue
                key = (root_name, str(path.resolve()).casefold())
                if key in seen:
                    continue
                if len(artifacts) >= 1000:
                    truncated = True
                    break
                seen.add(key)
                file_stat = path.stat()
                artifacts.append(
                    {
                        "root": root_name,
                        "path": path.relative_to(scoped.root).as_posix(),
                        "size": file_stat.st_size,
                        "modifiedNs": file_stat.st_mtime_ns,
                        "sha256": sha256_file(path),
                    }
                )
            if truncated:
                break
        artifacts.sort(key=lambda item: item["modifiedNs"], reverse=True)
        return {"artifacts": artifacts, "truncated": truncated}

    @staticmethod
    def _uat_artifact_path(command_name: str, arguments: list[str]) -> str | None:
        accepted = (
            ("package",) if command_name == "BuildPlugin" else ("archivedirectory", "stagingdirectory", "package")
        )
        values: dict[str, str] = {}
        for argument in arguments:
            option, separator, value = argument.partition("=")
            if not separator:
                continue
            name = option.lstrip("-/").casefold()
            if name in accepted and value.strip():
                values[name] = value.strip().strip("\"'")
        for name in accepted:
            if values.get(name):
                return values[name]
        return None

    @staticmethod
    def _normalized_unreal_version(value: str | None) -> str | None:
        if not value:
            return None
        match = re.search(r"(?<!\d)([45])\.(\d+)(?:\.\d+)?(?!\d)", value)
        return f"{match.group(1)}.{match.group(2)}" if match else None

    def _editor_executable(self) -> str | None:
        configured = self.configured.get("unrealEditor")
        if configured:
            path = Path(configured).expanduser()
            if path.is_dir():
                for name in ("UnrealEditor-Cmd.exe", "UE4Editor-Cmd.exe"):
                    candidate = path / name
                    if candidate.is_file():
                        return str(candidate.resolve())
            if path.is_file():
                return str(path.resolve())
        return self.executable("unrealEditor", ["UnrealEditor-Cmd.exe", "UE4Editor-Cmd.exe"])
