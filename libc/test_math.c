/* Host test for the x87 math functions. */
#include <stdio.h>
#include <math.h>

static int failures;

static void ck(const char *name, double got, double want)
{
	double d = got - want;

	if (d < 0)
		d = -d;
	if (!(d < 1e-9)) {
		printf("math mismatch %s: got %.17g want %.17g\n",
		       name, got, want);
		failures++;
	}
}

int main(void)
{
	int e;
	double ip;

	ck("sqrt", sqrt(9.0), 3.0);
	ck("fabs", fabs(-2.5), 2.5);
	ck("floor", floor(-1.2), -2.0);
	ck("ceil", ceil(-1.2), -1.0);
	ck("trunc", trunc(-1.2), -1.0);
	ck("round", round(2.5), 3.0);
	ck("fmod", fmod(7.5, 2.0), 1.5);
	ck("pow", pow(2.0, 10.0), 1024.0);
	ck("powneg", pow(-2.0, 3.0), -8.0);
	ck("powfrac", pow(9.0, 0.5), 3.0);
	ck("exp0", exp(0.0), 1.0);
	ck("exp1", exp(1.0), 2.718281828459045);
	ck("log1", log(1.0), 0.0);
	ck("logE", log(2.718281828459045), 1.0);
	ck("log10", log10(100.0), 2.0);
	ck("sin0", sin(0.0), 0.0);
	ck("cos0", cos(0.0), 1.0);
	ck("tan0", tan(0.0), 0.0);
	ck("atan1", atan(1.0), 0.7853981633974483);
	ck("atan2", atan2(1.0, 1.0), 0.7853981633974483);
	ck("asin", asin(0.5), 0.5235987755982989);
	ck("acos", acos(0.5), 1.0471975511965977);
	ck("ldexp", ldexp(1.5, 3), 12.0);
	ck("frexp", frexp(12.0, &e), 0.75);
	if (e != 4) {
		printf("math mismatch frexp exponent %d\n", e);
		failures++;
	}
	ck("modf", modf(2.75, &ip), 0.75);
	ck("modfip", ip, 2.0);
	ck("fmin", fmin(2.0, 3.0), 2.0);
	ck("fmax", fmax(2.0, 3.0), 3.0);
	ck("copysign", copysign(2.0, -1.0), -2.0);

	if (failures)
		printf("math test: %d failure(s)\n", failures);
	else
		printf("math test: OK\n");
	return failures != 0;
}
