# Third-Party Notices

**Project:** DirectMotion 3.0.0  
**Scope:** Third-party code and build/runtime dependencies identified in the DirectMotion 3.0.0 source package.

This document records the third-party components used by DirectMotion and the license notices relevant to the source package and its build/runtime model. It supplements the repository-root `LICENSE`; it does not replace that license or any copyright and license notices retained in individual files. Where a component carries its own license or copyright notice, that notice remains applicable.

## 1. DirectMotion

DirectMotion-authored project code is distributed under the GNU General Public License, version 3 (GPL-3.0), as stated in the repository-root `LICENSE`. Third-party components remain subject to their respective licenses and notices, as identified below or in the relevant source files.

## 2. Microsoft DirectShow Base Classes

**Component:** Microsoft DirectShow Base Classes  
**Repository location:** `common/baseclasses/`  
**Use:** The Base Classes source files and headers are compiled into the DirectMotion filter.

The source files retain Microsoft copyright headers, including the historical notice:

> Copyright (c) 1992-2001 Microsoft Corporation. All rights reserved.

The corresponding DirectShow Base Classes sample is available in Microsoft's `Windows-classic-samples` repository:

- Source: <https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/directshow/baseclasses>
- Repository license: <https://github.com/microsoft/Windows-classic-samples/blob/main/LICENSE>

The repository publishes the sample code under the MIT License, subject to any more specific notices included with individual samples or files. The MIT notice is reproduced below. Retain it, together with the original Microsoft copyright headers, when redistributing the reused source files or binaries containing them.

### MIT License — Microsoft Corporation

Copyright (c) Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## 3. AviSynth+

**Component:** AviSynth+  
**Version used for compile-time headers:** 3.7.5  
**Pinned upstream commit:** `6c7c26617a6675eec89e4d4a3565ed709df6511f`  
**Header:** `avs_core/include/avisynth.h`  
**Repository:** <https://github.com/AviSynth/AviSynthPlus>  
**Pinned source:** <https://github.com/AviSynth/AviSynthPlus/tree/6c7c26617a6675eec89e4d4a3565ed709df6511f>

The GitHub Actions workflow checks out the AviSynth+ 3.7.5 source revision identified above to obtain compile-time headers and verifies the pinned revision. The AviSynth+ headers and source tree are not copied into the DirectMotion source package, and the workflow does not bundle `avisynth.dll`.

The license header in `avisynth.h` identifies AviSynth+ as GPL-2.0-or-later and contains a special linking exception for independent modules that communicate with AviSynth solely through the interfaces defined in `avisynth.h`.

The exception permits distribution of the resulting combined work under the distributor's chosen terms, provided that **every copy of the combined work is accompanied by a complete copy of the source code of the exact AviSynth version used to produce it**, distributed under the GPL together with the exception. The exception defines an independent module as one that is not derived from or based on AviSynth, such as a third-party filter, import/export plugin, or graphical interface.

For the DirectMotion 3.0.0 build baseline, the relevant AviSynth+ source is the complete 3.7.5 source tree at the pinned commit above. A link to the upstream repository alone should not be treated as a substitute for the accompanying-source condition. The condition applies to distribution of a copy of the resulting combined work; it should not be read as a conclusion that every standalone file or every possible package is automatically such a copy. The treatment of a particular DirectMotion release package depends on the actual linking and distribution arrangement. Because the exception expressly addresses both static and dynamic linking, dynamic loading should not be assumed to avoid it. Before distributing a package that constitutes a copy of the combined work, confirm that DirectMotion meets the independent-module and interface-only conditions, and accompany each distributed copy with the complete AviSynth+ source specified above.

The authoritative license and exception text is in the pinned header:

- <https://github.com/AviSynth/AviSynthPlus/blob/6c7c26617a6675eec89e4d4a3565ed709df6511f/avs_core/include/avisynth.h>

## 4. MVTools2

**Component:** MVTools2 AviSynth plugin  
**Use:** Loaded at runtime through AviSynth+; DirectMotion invokes the functions exposed by the plugin.  
**Upstream project:** <https://github.com/pinterf/mvtools>  
**Upstream documentation:** <https://github.com/pinterf/mvtools/blob/mvtools-pfmod/Documentation/mvtools2.html>

MVTools2 is an external runtime dependency. Its source code and `mvtools2.dll` are not included in the DirectMotion source package or the GitHub Actions build artifact.

The upstream documentation identifies the plugin as GPL-licensed and states that some portions of its code are under the WTFPL. It also lists code derived from other projects, including SimpleResize, Xvid, FFTW, x264 and x265. Those component-specific notices and license terms remain relevant to any MVTools2 source or binary redistribution. The MVTools2 documentation itself is identified as Creative Commons Attribution-ShareAlike 3.0 (CC BY-SA 3.0); that license applies when reproducing or distributing the documentation.

This project does not grant additional rights to redistribute MVTools2. The exact MVTools2 runtime version used for a particular DirectMotion deployment must be identified separately. Before bundling `mvtools2.dll`, its source, or its documentation, verify the applicable licenses and notices for that exact version and all included components. Do not assume that DirectMotion's GPL-3.0 license replaces those upstream obligations.

## 5. Microsoft Windows SDK, MSVC and Windows APIs

The Windows x64 build uses the Microsoft Windows SDK and MSVC toolchain and calls Windows and DirectShow APIs. The SDK and compiler toolchain are build-environment dependencies; their binaries and license texts are not included in the DirectMotion source package. Their installation and use are subject to Microsoft's applicable terms.

## 6. Runtime components not included in the source package

The DirectMotion 3.0.0 minimal source package and GitHub Actions build artifact do not include these runtime binaries:

- `avisynth.dll`
- `mvtools2.dll`
- `avstp.dll`

The filter requires compatible AviSynth+ and MVTools2 runtime components to be supplied separately. `avstp.dll` is not required by the current graph configuration. Identify and verify the exact runtime files, versions, architecture, provenance and SHA-256 hashes before redistributing them.

## 7. Scope and updates

These notices describe the DirectMotion 3.0.0 source package and build configuration. They do not establish the license status of arbitrary AviSynth+ or MVTools2 binaries installed separately on a user's system.

When a future revision vendors, copies, links, or redistributes additional third-party code or documentation, update this file and preserve the corresponding upstream copyright, license and attribution notices before publishing that revision.

This document is an attribution and compliance aid, not a legal opinion. A legal review is appropriate before public or commercial binary distribution, particularly for the AviSynth+ accompanying-source condition and any separately supplied MVTools2 runtime.
