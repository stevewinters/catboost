/*
 * Wrapper module for libqhull, providing Delaunay triangulation.
 *
 * This module's methods should not be accessed directly.  To obtain a Delaunay
 * triangulation, construct an instance of the matplotlib.tri.Triangulation
 * class without specifying a triangles array.
 */
#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "numpy_cpp.h"
#ifdef _MSC_VER
/* The Qhull header does not declare this as extern "C", but only MSVC seems to
 * do name mangling on global variables. We thus need to declare this before
 * the header so that it treats it correctly, and doesn't mangle the name. */
extern "C" {
extern const char qh_version[];
}
#endif
#include "libqhull_r/qhull_ra.h"
#include <cstdio>
#include <vector>


#ifndef MPL_DEVNULL
#error "MPL_DEVNULL must be defined as the OS-equivalent of /dev/null"
#endif

#define STRINGIFY(x) STR(x)
#define STR(x) #x


static const char* qhull_error_msg[6] = {
    "",                     /* 0 = qh_ERRnone */
    "input inconsistency",  /* 1 = qh_ERRinput */
    "singular input data",  /* 2 = qh_ERRsingular */
    "precision error",      /* 3 = qh_ERRprec */
    "insufficient memory",  /* 4 = qh_ERRmem */
    "internal error"};      /* 5 = qh_ERRqhull */


/* Return the indices of the 3 vertices that comprise the specified facet (i.e.
 * triangle). */
static void
get_facet_vertices(qhT* qh, const facetT* facet, int indices[3])
{
    vertexT *vertex, **vertexp;
    FOREACHvertex_(facet->vertices) {
        *indices++ = qh_pointid(qh, vertex->point);
    }
}

/* Return the indices of the 3 triangles that are neighbors of the specified
 * facet (triangle). */
static void
get_facet_neighbours(const facetT* facet, std::vector<int>& tri_indices,
                     int indices[3])
{
    facetT *neighbor, **neighborp;
    FOREACHneighbor_(facet) {
        *indices++ = (neighbor->upperdelaunay ? -1 : tri_indices[neighbor->id]);
    }
}

/* Return true if the specified points arrays contain at least 3 unique points,
 * or false otherwise. */
static bool
at_least_3_unique_points(npy_intp npoints, const double* x, const double* y)
{
    int i;
    const int unique1 = 0;  /* First unique point has index 0. */
    int unique2 = 0;        /* Second unique point index is 0 until set. */

    if (npoints < 3) {
        return false;
    }

    for (i = 1; i < npoints; ++i) {
        if (unique2 == 0) {
            /* Looking for second unique point. */
            if (x[i] != x[unique1] || y[i] != y[unique1]) {
                unique2 = i;
            }
        }
        else {
            /* Looking for third unique point. */
            if ( (x[i] != x[unique1] || y[i] != y[unique1]) &&
                 (x[i] != x[unique2] || y[i] != y[unique2]) ) {
                /* 3 unique points found, with indices 0, unique2 and i. */
                return true;
            }
        }
    }

    /* Run out of points before 3 unique points found. */
    return false;
}

/* Holds on to info from Qhull so that it can be destructed automatically. */
class QhullInfo {
public:
    QhullInfo(FILE *error_file, qhT* qh) {
        this->error_file = error_file;
        this->qh = qh;
    }

    ~QhullInfo() {
        qh_freeqhull(this->qh, !qh_ALL);
        int curlong, totlong;  /* Memory remaining. */
        qh_memfreeshort(this->qh, &curlong, &totlong);
        if (curlong || totlong) {
            PyErr_WarnEx(PyExc_RuntimeWarning,
                         "Qhull could not free all allocated memory", 1);
        }

        if (this->error_file != stderr) {
            fclose(error_file);
        }
    }

private:
    FILE* error_file;
    qhT* qh;
};

/* Delaunay implementation method.
 * If hide_qhull_errors is true then qhull error messages are discarded;
 * if it is false then they are written to stderr. */
static PyObject*
delaunay_impl(npy_intp npoints, const double* x, const double* y,
              bool hide_qhull_errors)
{
    qhT qh_qh;                  /* qh variable type and name must be like */
    qhT* qh = &qh_qh;           /* this for Qhull macros to work correctly. */
    facetT* facet;
    int i, ntri, max_facet_id;
    int exitcode;               /* Value returned from qh_new_qhull(). */
    const int ndim = 2;
    double x_mean = 0.0;
    double y_mean = 0.0;

    QHULL_LIB_CHECK

    /* Allocate points. */
    std::vector<coordT> points(npoints * ndim);

    /* Determine mean x, y coordinates. */
    for (i = 0; i < npoints; ++i) {
        x_mean += x[i];
        y_mean += y[i];
    }
    x_mean /= npoints;
    y_mean /= npoints;

    /* Prepare points array to pass to qhull. */
    for (i = 0; i < npoints; ++i) {
        points[2*i  ] = x[i] - x_mean;
        points[2*i+1] = y[i] - y_mean;
    }

    /* qhull expects a FILE* to write errors to. */
    FILE* error_file = NULL;
    if (hide_qhull_errors) {
        /* qhull errors are ignored by writing to OS-equivalent of /dev/null.
         * Rather than have OS-specific code here, instead it is determined by
         * setupext.py and passed in via the macro MPL_DEVNULL. */
        error_file = fopen(STRINGIFY(MPL_DEVNULL), "w");
        if (error_file == NULL) {
            throw std::runtime_error("Could not open devnull");
        }
    }
    else {
        /* qhull errors written to stderr. */
        error_file = stderr;
    }

    /* Perform Delaunay triangulation. */
    QhullInfo info(error_file, qh);
    qh_zero(qh, error_file);
    exitcode = qh_new_qhull(qh, ndim, (int)npoints, points.data(), False,
                            (char*)"qhull d Qt Qbb Qc Qz", NULL, error_file);
    if (exitcode != qh_ERRnone) {
        PyErr_Format(PyExc_RuntimeError,
                     "Error in qhull Delaunay triangulation calculation: %s (exitcode=%d)%s",
                     qhull_error_msg[exitcode], exitcode,
                     hide_qhull_errors ? "; use python verbose option (-v) to see original qhull error." : "");
        return NULL;
    }

    /* Split facets so that they only have 3 points each. */
    qh_triangulate(qh);

    /* Determine ntri and max_facet_id.
       Note that libqhull uses macros to iterate through collections. */
    ntri = 0;
    FORALLfacets {
        if (!facet->upperdelaunay) {
            ++ntri;
        }
    }

    max_facet_id = qh->facet_id - 1;

    /* Create array to map facet id to triangle index. */
    std::vector<int> tri_indices(max_facet_id+1);

    /* Allocate Python arrays to return. */
    npy_intp dims[2] = {ntri, 3};
    numpy::array_view<int, ndim> triangles(dims);
    int* triangles_ptr = triangles.data();

    numpy::array_view<int, ndim> neighbors(dims);
    int* neighbors_ptr = neighbors.data();

    /* Determine triangles array and set tri_indices array. */
    i = 0;
    FORALLfacets {
        if (!facet->upperdelaunay) {
            int indices[3];
            tri_indices[facet->id] = i++;
            get_facet_vertices(qh, facet, indices);
            *triangles_ptr++ = (facet->toporient ? indices[0] : indices[2]);
            *triangles_ptr++ = indices[1];
            *triangles_ptr++ = (facet->toporient ? indices[2] : indices[0]);
        }
        else {
            tri_indices[facet->id] = -1;
        }
    }

    /* Determine neighbors array. */
    FORALLfacets {
        if (!facet->upperdelaunay) {
            int indices[3];
            get_facet_neighbours(facet, tri_indices, indices);
            *neighbors_ptr++ = (facet->toporient ? indices[2] : indices[0]);
            *neighbors_ptr++ = (facet->toporient ? indices[0] : indices[2]);
            *neighbors_ptr++ = indices[1];
        }
    }

    PyObject* tuple = PyTuple_New(2);
    if (tuple == 0) {
        throw std::runtime_error("Failed to create Python tuple");
    }

    PyTuple_SET_ITEM(tuple, 0, triangles.pyobj());
    PyTuple_SET_ITEM(tuple, 1, neighbors.pyobj());
    return tuple;
}

/* Process Python arguments and call Delaunay implementation method. */
static PyObject*
delaunay(PyObject *self, PyObject *args)
{
    numpy::array_view<double, 1> xarray;
    numpy::array_view<double, 1> yarray;
    PyObject* ret;
    npy_intp npoints;
    const double* x;
    const double* y;
    int verbose = 0;

    if (!PyArg_ParseTuple(args, "O&O&i:delaunay",
                          &xarray.converter_contiguous, &xarray,
                          &yarray.converter_contiguous, &yarray,
                          &verbose)) {
        return NULL;
    }

    npoints = xarray.dim(0);
    if (npoints != yarray.dim(0)) {
        PyErr_SetString(PyExc_ValueError,
                        "x and y must be 1D arrays of the same length");
        return NULL;
    }

    if (npoints < 3) {
        PyErr_SetString(PyExc_ValueError,
                        "x and y arrays must have a length of at least 3");
        return NULL;
    }

    x = xarray.data();
    y = yarray.data();

    if (!at_least_3_unique_points(npoints, x, y)) {
        PyErr_SetString(PyExc_ValueError,
                        "x and y arrays must consist of at least 3 unique points");
        return NULL;
    }

    CALL_CPP("qhull.delaunay",
             (ret = delaunay_impl(npoints, x, y, verbose == 0)));

    return ret;
}

/* Return qhull version string for assistance in debugging. */
static PyObject*
version(PyObject *self, PyObject *arg)
{
    return PyBytes_FromString(qh_version);
}

static PyMethodDef qhull_methods[] = {
    {"delaunay", delaunay, METH_VARARGS,
     "delaunay(x, y, verbose, /)\n"
     "--\n\n"
     "Compute a Delaunay triangulation.\n"
     "\n"
     "Parameters\n"
     "----------\n"
     "x, y : 1d arrays\n"
     "    The coordinates of the point set, which must consist of at least\n"
     "    three unique points.\n"
     "verbose : int\n"
     "    Python's verbosity level.\n"
     "\n"
     "Returns\n"
     "-------\n"
     "triangles, neighbors : int arrays, shape (ntri, 3)\n"
     "    Indices of triangle vertices and indices of triangle neighbors.\n"
    },
    {"version", version, METH_NOARGS,
     "version()\n--\n\n"
     "Return the qhull version string."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef qhull_module = {
    PyModuleDef_HEAD_INIT,
    "qhull", "Computing Delaunay triangulations.\n", -1, qhull_methods
};

#pragma GCC visibility push(default)

PyMODINIT_FUNC
PyInit__qhull(void)
{
    import_array();
    return PyModule_Create(&qhull_module);
}

#pragma GCC visibility pop
