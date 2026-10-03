## The C++17 special math functions (`msvcp140_2.dll`)

The last satellite of the C++ standard library the
[msvcp140 work](#the-c-standard-library-c99-complex-math-and-dll-directories)
left out.  Programs built by Visual Studio that call `<cmath>`'s special
functions (`std::cyl_bessel_j`, `std::expint`, `std::riemann_zeta`...)
import them from `msvcp140_2.dll` as `__std_smf_*`, and did not load on
NovaOS.  Nothing new was written: Microsoft builds the DLL from one STL
source, `special_math.cpp`, a thin wrapper over Boost.Math (Boost
Software License), and NovaOS now does the same.

- **`third_party/msstl/src/special_math.cpp`** joins the vendored STL
  (same `vs-2022-17.13` tag), and **`third_party/boost-math`** holds the
  110 Boost.Math headers it reaches, from the commit that STL tag pins,
  unchanged, with Boost's licence.  `userland/msvcp140_2/build.py`
  compiles it like the other satellites, with
  `BOOST_MATH_STANDALONE=1` as Microsoft's build does (no other Boost
  libraries needed).
- **The DLL exports the 44 `__std_smf_*` functions** (`double` and `f`
  forms; the `l` forms are inline in `<cmath>` and call the `double`
  ones) at ordinals 1 to 44 in name order, as the linker numbers
  Microsoft's, for 64- and 32-bit programs.  Outside a function's domain
  it returns NaN and sets the Universal C Runtime's `errno` to `EDOM`.
- **Self-test `smftest`** (64- and 32-bit) checks 28 values against
  closed forms and published constants (ζ(2) = π²/6, B(2, 3) = 1/12,
  J₀(1), K(0.5)...) and the `EDOM` case.  None of the nightly corpus
  programs or App Store downloads imports `msvcp140_2.dll` today, so the
  self-test is the proof.
- **Build fix**: changing a C++ program (`userland/**/*.cpp`) now
  rebuilds the userland; before, only C sources, headers and manifests
  were tracked.
