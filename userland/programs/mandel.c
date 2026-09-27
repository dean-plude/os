/* mandel.exe — ASCII Mandelbrot set (floating point in user mode) */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 78, h = argc > 2 ? atoi(argv[2]) : 24;
    const char *shade = " .:-=+*#%@";
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double cr = -2.2 + 3.2 * x / w, ci = -1.2 + 2.4 * y / h, zr = 0, zi = 0;
            int i = 0;
            while (i < 200 && zr * zr + zi * zi < 4) {
                double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
                i++;
            }
            putchar(i == 200 ? '@' : shade[i % 9]);
        }
        putchar('\n');
    }
    return 0;
}
