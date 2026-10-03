/* smftest.exe — the C++17 special math functions of <cmath> (std::beta,
 * std::cyl_bessel_j, std::riemann_zeta...), which MSVC-built programs
 * import from msvcp140_2.dll as __std_smf_*, built against Microsoft's
 * STL headers as Visual Studio builds a program */
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <windows.h>

static int pass, fail;

static void check(const char *what, double got, double want, double tol = 1e-12)
{
    if (std::fabs(got - want) <= tol * (std::fabs(want) > 1 ? std::fabs(want) : 1)) {
        pass++;
    } else {
        fail++;
        std::printf("FAIL: %s = %.17g, expected %.17g\n", what, got, want);
    }
}

int main()
{
    const double pi = 3.14159265358979323846;
    check("riemann_zeta(2)", std::riemann_zeta(2.0), pi * pi / 6);
    check("riemann_zeta(-1)", std::riemann_zeta(-1.0), -1.0 / 12);
    check("beta(2, 3)", std::beta(2.0, 3.0), 1.0 / 12);
    check("expint(1)", std::expint(1.0), 1.8951178163559368);
    check("cyl_bessel_j(0, 1)", std::cyl_bessel_j(0.0, 1.0), 0.76519768655796655);
    check("cyl_bessel_i(0, 1)", std::cyl_bessel_i(0.0, 1.0), 1.2660658777520082);
    check("cyl_bessel_k(0, 1)", std::cyl_bessel_k(0.0, 1.0), 0.42102443824070834);
    check("cyl_neumann(0, 1)", std::cyl_neumann(0.0, 1.0), 0.088256964215676956);
    check("sph_bessel(1, 1)", std::sph_bessel(1, 1.0), std::sin(1.0) - std::cos(1.0));
    check("sph_neumann(0, 1)", std::sph_neumann(0, 1.0), -std::cos(1.0));
    check("legendre(2, 0.5)", std::legendre(2, 0.5), -0.125);
    check("assoc_legendre(2, 1, 0.5)", std::assoc_legendre(2, 1, 0.5), 1.5 * std::sqrt(0.75));
    check("sph_legendre(1, 0, 0)", std::sph_legendre(1, 0, 0.0), std::sqrt(3 / (4 * pi)));
    check("hermite(3, 2)", std::hermite(3, 2.0), 40);
    check("laguerre(2, 1)", std::laguerre(2, 1.0), -0.5);
    check("assoc_laguerre(1, 2, 1)", std::assoc_laguerre(1, 2, 1.0), 2);
    check("comp_ellint_1(0)", std::comp_ellint_1(0.0), pi / 2);
    check("comp_ellint_1(0.5)", std::comp_ellint_1(0.5), 1.6857503548125961);
    check("comp_ellint_2(0.5)", std::comp_ellint_2(0.5), 1.4674622093394272);
    check("comp_ellint_3(0.5, 0)", std::comp_ellint_3(0.5, 0.0), 1.6857503548125961);
    check("ellint_1(0.5, pi/2)", std::ellint_1(0.5, pi / 2), 1.6857503548125961);
    check("ellint_2(0.5, pi/2)", std::ellint_2(0.5, pi / 2), 1.4674622093394272);
    check("ellint_3(0.5, 0, pi/2)", std::ellint_3(0.5, 0.0, pi / 2), 1.6857503548125961);
    /* the float and long double forms */
    check("riemann_zetaf(2)", std::riemann_zetaf(2.0f), pi * pi / 6, 1e-6);
    check("cyl_bessel_jf(1, 2.5)", std::cyl_bessel_jf(1.0f, 2.5f), 0.49709410246427494, 1e-6);
    check("expintf(2)", std::expintf(2.0f), 4.9542343560018901, 1e-6);
    check("betal(2, 3)", (double)std::betal(2.0L, 3.0L), 1.0 / 12);
    check("hermitel(3, 2)", (double)std::hermitel(3, 2.0L), 40);
    /* outside the domain: NaN and EDOM.  A Visual Studio program's errno
     * is the Universal C Runtime's, which msvcp140_2.dll sets; this
     * program's C runtime is msvcrt.dll, so it reads ucrtbase.dll's */
    int *(*ucrt_errno)(void) = (int *(*)(void))GetProcAddress(GetModuleHandleA("ucrtbase.dll"), "_errno");
    *ucrt_errno() = 0;
    double d = std::legendre(2, 2.0);
    if (std::isnan(d) && *ucrt_errno() == EDOM) {
        pass++;
    } else {
        fail++;
        std::printf("FAIL: legendre(2, 2) = %g, errno %d\n", d, *ucrt_errno());
    }
    std::printf("smftest: cyl_bessel_j(0, 1) = %.6f\n", std::cyl_bessel_j(0.0, 1.0));
    std::printf("smftest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
