"""Loading and validation of the repository-local zOS toolchain."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Toolchain:
    Root: Path
    Bin: Path
    Clang: Path
    ClangXX: Path
    Lld: Path
    LldLink: Path
    LlvmNm: Path
    LlvmReadObj: Path
    Qemu: Path
    OvmfCode: Path
    OvmfVariablesTemplate: Path
    QemuAccelerator: str

    @classmethod
    def Load(cls, repository_root: Path) -> "Toolchain": 
        root = repository_root / ".zos" / "Toolchain"
        binary_directory = root / "bin"
        manifest_path = root / "Manifest.json"
        if not manifest_path.is_file():
            raise RuntimeError(
                "The zOS toolchain manifest is missing. Run "
                "./Tools/SetupDevEnv.sh first."
            )

        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        qemu = manifest["toolchain"]["qemu"]
        ovmf = manifest["toolchain"]["ovmf"]

        toolchain = cls(
            Root=root,
            Bin=binary_directory,
            Clang=binary_directory / "clang",
            ClangXX=binary_directory / "clang++",
            Lld=binary_directory / "ld.lld",
            LldLink=binary_directory / "lld-link",
            LlvmNm=binary_directory / "llvm-nm",
            LlvmReadObj=binary_directory / "llvm-readobj",
            Qemu=binary_directory / "qemu-system-x86_64",
            OvmfCode=Path(ovmf["code"]),
            OvmfVariablesTemplate=Path(ovmf["variablesTemplate"]),
            QemuAccelerator=qemu.get("accelerator") or "tcg,thread=multi",
        )
        toolchain.Validate()
        return toolchain

    def Validate(self) -> None:
        required_files = {
            "Clang": self.Clang,
            "Clang++": self.ClangXX,
            "LLD": self.Lld,
            "lld-link": self.LldLink,
            "llvm-nm": self.LlvmNm,
            "llvm-readobj": self.LlvmReadObj,
            "QEMU": self.Qemu,
            "OVMF code": self.OvmfCode,
            "OVMF variables template": self.OvmfVariablesTemplate,
        }

        missing = [f"{name}: {path}" for name, path in required_files.items() if not path.is_file()]
        if missing:
            raise RuntimeError("Required toolchain files are missing:\n  " + "\n  ".join(missing))

    
