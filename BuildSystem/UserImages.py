"""Common native ELF64 application build rules; no application policy here."""

from pathlib import Path
from SCons.Script import File

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}


def BuildRuntimeObjects(environment, output_root, repo_root):
    directory = Path(repo_root) / "User" / "Runtime"
    sources = sorted(path for path in directory.rglob("*") if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES)
    if not sources:
        raise RuntimeError("Native user runtime contains no translation units.")
    return [environment.Object(
        target=f"{output_root}/User/Runtime/{source.relative_to(directory).with_suffix(environment['OBJSUFFIX']).as_posix()}",
        source=File(str(source)),
    ) for source in sources]


def BuildNativeElf(environment, output_root, output_name, source_paths, runtime_objects, linker_script):
    objects = []
    for source_path in source_paths:
        name = Path(str(source_path)).stem
        objects.append(environment.Object(
            target=f"{output_root}/User/{output_name}/Objects/{name}{environment['OBJSUFFIX']}",
            source=File(str(source_path)),
        ))
    program = environment.Command(
        target=f"{output_root}/User/{output_name}/{output_name}.elf",
        source=[*objects, *runtime_objects],
        action=("$LINK $USER_LINKFLAGS --script=$USER_LINKER_SCRIPT --entry=_start -o $TARGET $SOURCES"),
        USER_LINKER_SCRIPT=linker_script.abspath,
    )
    environment.Depends(program, linker_script)
    return program
