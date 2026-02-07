// Dummy C extension to force platform-specific wheel generation.
// Without a compiled extension, setuptools would produce a pure-Python
// "py3-none-any" wheel, but we need a platform wheel because we ship
// a native CDO binary and shared libraries.
#include <Python.h>

static PyObject *_cdo_noop(PyObject *self, PyObject *args)
{
    Py_RETURN_NONE;
}

static PyMethodDef _cdo_methods[] = {
    {"_noop", _cdo_noop, METH_VARARGS, "No-op function for platform wheel generation"},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef _cdo_platform_module = {
    PyModuleDef_HEAD_INIT,
    "_cdo_platform",
    "Dummy extension to make this a platform-specific wheel",
    -1,
    _cdo_methods};

PyMODINIT_FUNC PyInit__cdo_platform(void)
{
    return PyModule_Create(&_cdo_platform_module);
}
