![Slice it All](resources/icons/SliceItAll_192px.png)

# Slice it All

Slice it All is a free 3D slicer with one goal: work for everyone, and for everything they print.

People print on machines from every workshop and every brand. They print with filament and with resin. Some printers sit on the network, and some never connect to anything. Slice it All is the slicer for all of them. One program prepares the print, and the file you get is yours.

It is free software. Free to download, free to use on as many computers as you like, and free to study and improve. The license is the GNU Affero General Public License, version 3, which keeps those freedoms attached to the program.

## Install

The Windows installer for version 0.9.3 is on the [release page](https://github.com/synchleague/SliceItAll/releases/tag/v0.9.3).

Run `SliceItAll-0.9.3-win64-setup.exe`. It installs Slice it All, adds a Start menu entry, and can add a desktop shortcut. Uninstall it from Windows Settings whenever you want.

`SliceItAll-console.exe` is installed beside the app and slices from the command line.

## Printing

- Load STL, OBJ, 3MF, and STEP models.
- Slice for filament printers and for resin printers.
- Start from a printer profile, or describe a machine yourself.
- Preview toolpaths and resin layers before you export.
- Save G-code for filament printers, and the layer file a resin printer expects.
- Keep working when the printer is offline. Slice it All will still write the file.

## The idea

A slicer is a tool for making a physical thing. It should be available to anyone with a printer, and it should speak the language of that printer, whoever built it. Slice it All is that tool: universal, and free.

## Where it comes from

Slice it All is built on [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is built on [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

## License

[GNU Affero General Public License, version 3](LICENSE).

## Building from source

- [Windows](doc/How%20to%20build%20-%20Windows.md)
- [Linux](doc/How%20to%20build%20-%20Linux%20et%20al.md)
- [macOS](doc/How%20to%20build%20-%20Mac%20OS.md)

The Windows installer is built with Inno Setup. From the repository root:

```
powershell -ExecutionPolicy Bypass -File packaging\windows\package.ps1
```

That writes `dist\SliceItAll-0.9.3-win64-setup.exe`.
