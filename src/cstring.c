#include <Python.h>

#define WHITESPACE_CHARS    " \t\n\v\f\r"

/* memrchr not available on some systems, so reimplement. */
const char *_memrchr(const char *s, int c, size_t n) {
    for(const char *p = s + n - 1; p >= s; --p) {
        if(*p == c)
            return p;
    }
    return NULL;
}

const char *_strrstr(const char *s, const char *find) {
    const char *p = s + strlen(s) - 1;
    for(;p > s; --p) {
        if(memcmp(p, find, strlen(find)) == 0)
            return p;
    }
    return NULL;
}


struct cstring {
    PyObject_VAR_HEAD
    Py_hash_t hash;
    char value[];
};

static PyTypeObject cstring_type;

#define CSTRING_HASH(self)          (((struct cstring *)self)->hash)
#define CSTRING_VALUE(self)         (((struct cstring *)self)->value)
#define CSTRING_VALUE_AT(self, i)   (&CSTRING_VALUE(self)[(i)])
#define CSTRING_LAST_BYTE(self)     (CSTRING_VALUE(self)[Py_SIZE(self) - 1])

#define CSTRING_ALLOC(tp, len)      ((struct cstring *)(tp)->tp_alloc((tp), (len)))

/* singleton, initialized in cstring_new_empty */
static const struct cstring *cstring_EMPTY = NULL;

static void *_bad_argument_type(PyObject *o) {
    PyErr_Format(
        PyExc_TypeError,
        "Bad argument type: %s",
        Py_TYPE(o)->tp_name);
    return NULL;
}

static PyObject *_cstring_new(PyTypeObject *type, const char *value, Py_ssize_t len) {
    struct cstring *new = CSTRING_ALLOC(type, len + 1);
    if(!new)
        return NULL;
    new->hash = -1;
    memcpy(new->value, value, len);
    CSTRING_LAST_BYTE(new) = '\0';
    return (PyObject *)new;
}

static PyObject *_cstring_realloc(PyObject *self, Py_ssize_t len) {
    if(Py_REFCNT(self) > 1)
        return PyErr_BadInternalCall(), NULL;
    struct cstring *new = PyObject_Realloc(self, sizeof(struct cstring) + len + 1);
    if(!new)
        return PyErr_NoMemory();
    Py_SET_SIZE(new, len + 1);
    new->hash = -1;
    return (PyObject *)new;
}

static PyObject *_cstring_copy(PyObject *self) {
    return _cstring_new(Py_TYPE(self), CSTRING_VALUE(self), Py_SIZE(self) - 1);
}

static PyObject *cstring_new_empty(void) {
    if(!cstring_EMPTY) {
        cstring_EMPTY = (struct cstring *)_cstring_new(&cstring_type, "", 0);
    }
    /* leaking one reference for singleton cache (never cleaned up) */
    Py_INCREF(cstring_EMPTY);
    return (PyObject *)cstring_EMPTY;
}

static const char *_obj_as_string_and_size(PyObject *o, Py_ssize_t *s) {
    if(PyUnicode_Check(o))
        return PyUnicode_AsUTF8AndSize(o, s);

    if(PyObject_CheckBuffer(o)) {
        /* handles bytes, bytearrays, arrays, memoryviews, etc. */
        Py_buffer view;
        if(PyObject_GetBuffer(o, &view, PyBUF_SIMPLE) < 0)
            return NULL;
        *s = view.len;
        const char *buffer = view.buf;
        PyBuffer_Release(&view);
        return buffer;
    }

    if(PyObject_TypeCheck(o, &cstring_type)) {
        /* TODO: implement buffer protocol for cstring */
        *s = Py_SIZE(o) - 1;
        return CSTRING_VALUE(o);
    }

    *s = -1;
    return _bad_argument_type(o);
}

static PyObject *cstring_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    PyObject *argobj = NULL;
    if(!PyArg_ParseTuple(args, "O", &argobj))
        return NULL;

    if(PyObject_TypeCheck(argobj, type)) {
        Py_INCREF(argobj);
        return argobj;
    }

    Py_ssize_t len = 0;
    const char *buffer = _obj_as_string_and_size(argobj, &len);
    if(!buffer)
        return NULL;

    if(len == 0)
        return cstring_new_empty();

    return _cstring_new(type, buffer, len);
}

static void cstring_dealloc(PyObject *self) {
    Py_TYPE(self)->tp_free(self);
}

static int _ensure_cstring(PyObject *self) {
    if(PyObject_TypeCheck(self, &cstring_type))
        return 1;
    PyErr_Format(
        PyExc_TypeError,
        "Object must have type cstring, not %s.",
        Py_TYPE(self)->tp_name);
    return 0;
}

static PyObject *cstring_str(PyObject *self) {
    return PyUnicode_FromString(CSTRING_VALUE(self));
}

static PyObject *cstring_repr(PyObject *self) {
    PyObject *tmp = cstring_str(self);
    PyObject *repr = PyObject_Repr(tmp);
    Py_DECREF(tmp);
    return repr;
}

static Py_hash_t cstring_hash(PyObject *self) {
    if(CSTRING_HASH(self) == -1)
        CSTRING_HASH(self) = _Py_HashBytes(CSTRING_VALUE(self), Py_SIZE(self));
    return CSTRING_HASH(self);
}

static PyObject *cstring_richcompare(PyObject *self, PyObject *other, int op) {
    if(!_ensure_cstring(other))
        return NULL;

    const char *left = CSTRING_VALUE(self);
    const char *right = CSTRING_VALUE(other);

    for(;*left && *right && *left == *right; ++left, ++right)
        ;

    switch (op) {
    case Py_EQ:
        return PyBool_FromLong(*left == *right);
    case Py_NE:
        return PyBool_FromLong(*left != *right);
    case Py_LT:
        return PyBool_FromLong(*left < *right);
    case Py_GT:
        return PyBool_FromLong(*left > *right);
    case Py_LE:
        return PyBool_FromLong(*left <= *right);
    case Py_GE:
        return PyBool_FromLong(*left >= *right);
    default:
        Py_UNREACHABLE();
    }
}

static Py_ssize_t cstring_len(PyObject *self) {
    return Py_SIZE(self) - 1;
}

static PyObject *_concat_in_place(PyObject *self, PyObject *other) {
    if(!other)
        return PyErr_BadArgument(), NULL;
    if(!_ensure_cstring(other))
        return NULL;
    if(!self)
        return _cstring_copy(other);  /* new (mutable) copy with refcnt=1 */
    if(!_ensure_cstring(self))
        return NULL;

    Py_ssize_t origlen = cstring_len(self);
    Py_ssize_t newlen = origlen + cstring_len(other);
    PyObject *new = _cstring_realloc(self, newlen);
    if(!new)
        return NULL;

    memcpy(CSTRING_VALUE_AT(new, origlen), CSTRING_VALUE(other), cstring_len(other));
    CSTRING_LAST_BYTE(new) = '\0';
    return new;
}

static PyObject *cstring_concat(PyObject *left, PyObject *right) {
    if(!_ensure_cstring(left))
        return NULL;
    if(!_ensure_cstring(right))
        return NULL;

    Py_ssize_t size = cstring_len(left) + cstring_len(right) + 1;

    struct cstring *new = CSTRING_ALLOC(Py_TYPE(left), size);
    if(!new)
        return NULL;
    memcpy(new->value, CSTRING_VALUE(left), Py_SIZE(left));
    memcpy(&new->value[cstring_len(left)], CSTRING_VALUE(right), Py_SIZE(right)); 
    return (PyObject *)new;
}

static PyObject *cstring_repeat(PyObject *self, Py_ssize_t count) {
    if(!_ensure_cstring(self))
        return NULL;
    if(count <= 0)
        return cstring_new_empty();

    Py_ssize_t size = (cstring_len(self) * count) + 1;

    struct cstring *new = CSTRING_ALLOC(Py_TYPE(self), size);
    if(!new)
        return NULL;
    for(Py_ssize_t i = 0; i < size - 1; i += cstring_len(self)) {
        memcpy(&new->value[i], CSTRING_VALUE(self), Py_SIZE(self));
    }
    return (PyObject *)new;
}

static Py_ssize_t _ensure_valid_index(PyObject *self, Py_ssize_t i) {
    if(i >= 0 && i < cstring_len(self))
        return i;
    PyErr_SetString(PyExc_IndexError, "Index is out of bounds");
    return -1;
}

static PyObject *cstring_item(PyObject *self, Py_ssize_t i) {
    if(_ensure_valid_index(self, i) < 0)
        return NULL;
    return _cstring_new(Py_TYPE(self), CSTRING_VALUE_AT(self, i), 1);
}

static int cstring_contains(PyObject *self, PyObject *arg) {
    if(!_ensure_cstring(arg))
        return -1;
    if(strstr(CSTRING_VALUE(self), CSTRING_VALUE(arg)))
        return 1;
    return 0;
}

static PyObject *_cstring_subscript_index(PyObject *self, PyObject *index) {
    Py_ssize_t i = PyNumber_AsSsize_t(index, PyExc_IndexError);
    if(PyErr_Occurred())
        return NULL;
    if(i < 0)
        i += cstring_len(self);
    return cstring_item(self, i);
}

static PyObject *_cstring_subscript_slice(PyObject *self, PyObject *slice) {
    Py_ssize_t start, stop, step;
    if(PySlice_Unpack(slice, &start, &stop, &step) < 0)
        return NULL;

    Py_ssize_t slicelen = PySlice_AdjustIndices(cstring_len(self), &start, &stop, step);
    struct cstring *new = CSTRING_ALLOC(Py_TYPE(self), slicelen + 1);
    if(!new)
        return NULL;

    char *src = CSTRING_VALUE_AT(self, start);
    for(Py_ssize_t i = 0; i < slicelen; ++i) {
        new->value[i] = *src;
        src += step;
    }
    new->value[slicelen] = '\0';
    return (PyObject *)new;
}

static PyObject *cstring_subscript(PyObject *self, PyObject *key) {
    if(PyIndex_Check(key))
        return _cstring_subscript_index(self, key);
    if(PySlice_Check(key))
        return _cstring_subscript_slice(self, key);

    PyErr_SetString(PyExc_TypeError, "Subscript must be int or slice.");
    return NULL;
}

static Py_ssize_t _fix_index(Py_ssize_t i, Py_ssize_t len) {
    Py_ssize_t result = i;
    if(result < 0)
        result += len;
    if(result < 0)
        result = 0;
    if(result > len)
        result = len;
    return result;
}

struct _substr_params {
    const char *start;
    const char *end;
    const char *substr;
    Py_ssize_t substr_len;
};

static struct _substr_params *_parse_substr_args(PyObject *self, PyObject *args, struct _substr_params *params) {
    PyObject *substr_obj;
    Py_ssize_t start = 0;
    Py_ssize_t end = PY_SSIZE_T_MAX;

    if(!PyArg_ParseTuple(args, "O|nn", &substr_obj, &start, &end))
        return NULL;

    Py_ssize_t substr_len;
    const char *substr = _obj_as_string_and_size(substr_obj, &substr_len);
    if(!substr)
        return NULL;

    start = _fix_index(start, cstring_len(self));
    end = _fix_index(end, cstring_len(self));

    params->start = CSTRING_VALUE_AT(self, start);
    params->end = CSTRING_VALUE_AT(self, end);
    params->substr = substr;
    params->substr_len = substr_len;

    return params;
}

PyDoc_STRVAR(count__doc__, "");
static PyObject *cstring_count(PyObject *self, PyObject *args) {
    struct _substr_params params;

    if(!_parse_substr_args(self, args, &params))
        return NULL;

    const char *p = params.start;
    long result = 0;
    while((p = strstr(p, params.substr)) != NULL) {
        ++result;
        p += params.substr_len;
        if(p >= params.end)
            break;
    }

    return PyLong_FromLong(result);
}

static const char *_substr_params_str(const struct _substr_params *params) {
    const char *p = strstr(params->start, params->substr);
    if(!p || p + params->substr_len > params->end)
        return NULL;
    return p;
}

static const char *_substr_params_rstr(const struct _substr_params *params) {
    const char *p = params->end - params->substr_len + 1;
    for(;;) {
        p = _memrchr(params->start, *params->substr, p - params->start);
        if(!p)
            goto done;
        if(memcmp(p, params->substr, params->substr_len) == 0)
            return p;
    }
done:
    return NULL;
}

PyDoc_STRVAR(find__doc__, "");
PyObject *cstring_find(PyObject *self, PyObject *args) {
    struct _substr_params params;

    if(!_parse_substr_args(self, args, &params))
        return NULL;

    const char *p = _substr_params_str(&params);
    if(!p)
        return PyLong_FromLong(-1);

    return PyLong_FromSsize_t(p - CSTRING_VALUE(self));
}

PyDoc_STRVAR(index__doc__, "");
PyObject *cstring_index(PyObject *self, PyObject *args) {
    struct _substr_params params;

    if(!_parse_substr_args(self, args, &params))
        return NULL;

    const char *p = _substr_params_str(&params);
    if(!p) {
        PyErr_SetString(PyExc_ValueError, "substring not found");
        return NULL;
    }

    return PyLong_FromSsize_t(p - CSTRING_VALUE(self));
}

PyDoc_STRVAR(isalnum__doc__, "");
PyObject *cstring_isalnum(PyObject *self, PyObject *args) {
    const char *p = CSTRING_VALUE(self);
    while(*p) {
        if(!isalnum(*p))
            Py_RETURN_FALSE;
        ++p;
    }
    Py_RETURN_TRUE;
}

PyDoc_STRVAR(isalpha__doc__, "");
PyObject *cstring_isalpha(PyObject *self, PyObject *args) {
    const char *p = CSTRING_VALUE(self);
    while(*p) {
        if(!isalpha(*p))
            Py_RETURN_FALSE;
        ++p;
    }
    Py_RETURN_TRUE;
}

PyDoc_STRVAR(isdigit__doc__, "");
PyObject *cstring_isdigit(PyObject *self, PyObject *args) {
    const char *p = CSTRING_VALUE(self);
    while(*p) {
        if(!isdigit(*p))
            Py_RETURN_FALSE;
        ++p;
    }
    Py_RETURN_TRUE;
}

PyDoc_STRVAR(islower__doc__, "");
PyObject *cstring_islower(PyObject *self, PyObject *args) {
    const char *p = CSTRING_VALUE(self);
    while(*p) {
        if(isalpha(*p)) {
            if(!islower(*p))
                Py_RETURN_FALSE;
            ++p;
            while(*p) {
                if(isalpha(*p) && !islower(*p))
                    Py_RETURN_FALSE;
                ++p;
            }
            /* at least one lc alpha and no uc alphas */
            Py_RETURN_TRUE;
        }
        ++p;
    }
    Py_RETURN_FALSE;
}

PyDoc_STRVAR(isprintable__doc__, "");
PyObject *cstring_isprintable(PyObject *self, PyObject *args) {
    const char *p = CSTRING_VALUE(self);
    while(*p) {
        if(!isprint(*p))
            Py_RETURN_FALSE;
        ++p;
    }
    Py_RETURN_TRUE;
}

PyDoc_STRVAR(isspace__doc__, "");
PyObject *cstring_isspace(PyObject *self, PyObject *args) {
    const char *p = CSTRING_VALUE(self);
    while(*p) {
        if(!isspace(*p))
            Py_RETURN_FALSE;
        ++p;
    }
    return PyBool_FromLong(p != CSTRING_VALUE(self));
}

PyDoc_STRVAR(isupper__doc__, "");
PyObject *cstring_isupper(PyObject *self, PyObject *args) {
    const char *p = CSTRING_VALUE(self);
    while(*p) {
        if(isalpha(*p)) {
            if(!isupper(*p))
                Py_RETURN_FALSE;
            ++p;
            while(*p) {
                if(isalpha(*p) && !isupper(*p))
                    Py_RETURN_FALSE;
                ++p;
            }
            /* at least one uc alpha and no lc alphas */
            Py_RETURN_TRUE;
        }
        ++p;
    }
    Py_RETURN_FALSE;
}

PyDoc_STRVAR(join__doc__, "");
PyObject *cstring_join(PyObject *self, PyObject *arg) {
    PyObject *iter = PyObject_GetIter(arg);
    if(!iter)
        return NULL;

    PyObject *result = NULL;
    PyObject *item = NULL;

    while((item = PyIter_Next(iter)) != NULL) {
        if(result) {
            PyObject *next = _concat_in_place(result, self);
            if(!next)
                goto fail;
            result = next;
        }
        PyObject *next = _concat_in_place(result, item);
        if(!next)
            goto fail;
        Py_DECREF(item);
        result = next;
    }

    return result;

fail:
    Py_XDECREF(item);
    Py_XDECREF(result);
    return NULL;
}

PyDoc_STRVAR(lower__doc__, "");
PyObject *cstring_lower(PyObject *self, PyObject *args) {
    struct cstring *new = CSTRING_ALLOC(Py_TYPE(self), Py_SIZE(self));
    if(!new)
        return NULL;
    const char *s = CSTRING_VALUE(self);
    char *d = CSTRING_VALUE(new);

    while((*d++ = tolower(*s++)) != '\0')
        ;

    return (PyObject *)new;
}

static PyObject *_tuple_steal_refs(Py_ssize_t count, ...) {
    PyObject *result = PyTuple_New(count);
    if(!result)
        return NULL;

    va_list va;
    va_start(va, count);
    for(int i = 0; i < count; ++i) {
        PyObject *o = va_arg(va, PyObject *);
        if(!o)
            goto fail;
        PyTuple_SET_ITEM(result, i, o);
    }
    va_end(va);

    return result;

fail:
    Py_DECREF(result);
    return NULL;
}

PyDoc_STRVAR(partition__doc__, "");
PyObject *cstring_partition(PyObject *self, PyObject *arg) {
    if(!_ensure_cstring(arg))
        return NULL;

    const char *search = CSTRING_VALUE(arg);

    const char *left = CSTRING_VALUE(self);
    const char *mid = strstr(left, search);
    if(!mid) {
        return _tuple_steal_refs(3,
            (Py_INCREF(self), self),
            cstring_new_empty(),
            cstring_new_empty());
    }
    const char *right = mid + strlen(search);

    return _tuple_steal_refs(3,
        _cstring_new(Py_TYPE(self), left, mid - left),
        _cstring_new(Py_TYPE(self), mid, right - mid),
        _cstring_new(Py_TYPE(self), right, &CSTRING_LAST_BYTE(self) - right));
}

PyDoc_STRVAR(rpartition__doc__, "");
PyObject *cstring_rpartition(PyObject *self, PyObject *arg) {
    if(!_ensure_cstring(arg))
        return NULL;

    const char *search = CSTRING_VALUE(arg);

    const char *left = CSTRING_VALUE(self);
    const char *mid = _strrstr(left, search);
    if(!mid) {
        return _tuple_steal_refs(3,
            cstring_new_empty(),
            cstring_new_empty(),
            (Py_INCREF(self), self));
    }
    const char *right = mid + strlen(search);

    return _tuple_steal_refs(3,
        _cstring_new(Py_TYPE(self), left, mid - left),
        _cstring_new(Py_TYPE(self), mid, right - mid),
        _cstring_new(Py_TYPE(self), right, &CSTRING_LAST_BYTE(self) - right));
}

PyDoc_STRVAR(rfind__doc__, "");
PyObject *cstring_rfind(PyObject *self, PyObject *args) {
    struct _substr_params params;

    if(!_parse_substr_args(self, args, &params))
        return NULL;

    const char *p = _substr_params_rstr(&params);

    if(!p)
        return PyLong_FromLong(-1);

    return PyLong_FromSsize_t(p - CSTRING_VALUE(self));
}

PyDoc_STRVAR(rindex__doc__, "");
PyObject *cstring_rindex(PyObject *self, PyObject *args) {
    struct _substr_params params;

    if(!_parse_substr_args(self, args, &params))
        return NULL;

    const char *p = _substr_params_rstr(&params);
    if(!p) {
        PyErr_SetString(PyExc_ValueError, "substring not found");
        return NULL;
    }

    return PyLong_FromSsize_t(p - CSTRING_VALUE(self));
}

PyObject *_cstring_split_on_chars(PyObject *self, const char seps[], Py_ssize_t maxsplit) {
    if(maxsplit < 0)
        maxsplit = PY_SSIZE_T_MAX;

    const char *start = CSTRING_VALUE(self);

    PyObject *list = PyList_New(0);
    if(!list)
        return NULL;

    while(*start) {
        const char *end = start;
        while(*end && !strchr(seps, *end))
            ++end;

        PyObject *new = _cstring_new(Py_TYPE(self), start, end - start);
        if(!new)
            goto fail;
        PyList_Append(list, new);
        Py_DECREF(new);

        const char *skip = end + 1;
        while(*skip && strchr(seps, *skip))
            ++skip;
        start = skip;

        if(PyList_GET_SIZE(list) + 1 > maxsplit) {
            PyObject *new = _cstring_new(Py_TYPE(self), start, strlen(start));
            if(!new)
                goto fail;
            PyList_Append(list, new);
            Py_DECREF(new);
            break;
        }
    }

    return list;

fail:
    Py_DECREF(list);
    return NULL;
}

PyObject *_cstring_split_on_cstring(PyObject *self, PyObject *sepobj, Py_ssize_t maxsplit) {
    if(!_ensure_cstring(sepobj))
        return NULL;

    if(maxsplit < 0)
        maxsplit = PY_SSIZE_T_MAX;

    PyObject *list = PyList_New(0);
    if(!list)
        return NULL;

    const char *sep = CSTRING_VALUE(sepobj);
    const char *s = CSTRING_VALUE(self);
    for(;;) {
        const char *e = strstr(s, sep);
        if(!e)
            break;
        PyObject *new = _cstring_new(Py_TYPE(self), s, e - s);
        if(!new)
            goto fail;
        PyList_Append(list, new);
        Py_DECREF(new);
        s = e + strlen(sep);
        if(PyList_GET_SIZE(list) + 1 > maxsplit)
            break;
    }

    PyObject *new = _cstring_new(Py_TYPE(self), s, strlen(s));
    if(!new)
        goto fail;
    PyList_Append(list, new);

    return list;

fail:
    Py_DECREF(list);
    return NULL;
}

PyDoc_STRVAR(split__doc__, "");
PyObject *cstring_split(PyObject *self, PyObject *args, PyObject *kwargs) {
    PyObject *sepobj = Py_None;
    int maxsplit = -1;
    char *kwlist[] = {"sep", "maxsplit", NULL};
    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "|Oi", kwlist, &sepobj, &maxsplit))
        return NULL;

    return (sepobj == Py_None)
        ? _cstring_split_on_chars(self, WHITESPACE_CHARS, maxsplit)
        : _cstring_split_on_cstring(self, sepobj, maxsplit);
}

PyDoc_STRVAR(startswith__doc__, "");
PyObject *cstring_startswith(PyObject *self, PyObject *args) {
    struct _substr_params params;
    if(!_parse_substr_args(self, args, &params))
        return NULL;
    if(params.end - params.start < params.substr_len)
        return PyBool_FromLong(0);
    int cmp = memcmp(params.start, params.substr, params.substr_len);
    return PyBool_FromLong(cmp == 0);
}

const char *_strip_chars_from_args(PyObject *args) {
    PyObject *charsobj = NULL;
    if(!PyArg_ParseTuple(args, "|O", &charsobj))
        return NULL;

    const char *chars = WHITESPACE_CHARS;

    if(charsobj && charsobj != Py_None) {
        if(!PyUnicode_Check(charsobj))
            return _bad_argument_type(charsobj);
        chars = PyUnicode_AsUTF8(charsobj);
    }

    return chars;
}

PyDoc_STRVAR(strip__doc__, "");
PyObject *cstring_strip(PyObject *self, PyObject *args) {
    const char *chars = _strip_chars_from_args(args);

    const char *start = CSTRING_VALUE(self);
    while(strchr(chars, *start))
        ++start;

    const char *end = &CSTRING_LAST_BYTE(self) - 1;
    while(strchr(chars, *end))
        --end;

    Py_ssize_t newsize = end - start + 1;
    return _cstring_new(Py_TYPE(self), start, newsize);
}

PyDoc_STRVAR(lstrip__doc__, "");
PyObject *cstring_lstrip(PyObject *self, PyObject *args) {
    const char *chars = _strip_chars_from_args(args);

    const char *start = CSTRING_VALUE(self);
    while(strchr(chars, *start))
        ++start;
    const char *end = &CSTRING_LAST_BYTE(self) - 1;

    Py_ssize_t newsize = end - start + 1;
    return _cstring_new(Py_TYPE(self), start, newsize);
}

PyDoc_STRVAR(rstrip__doc__, "");
PyObject *cstring_rstrip(PyObject *self, PyObject *args) {
    const char *chars = _strip_chars_from_args(args);

    const char *start = CSTRING_VALUE(self);
    const char *end = &CSTRING_LAST_BYTE(self) - 1;
    while(strchr(chars, *end))
        --end;

    Py_ssize_t newsize = end - start + 1;
    return _cstring_new(Py_TYPE(self), start, newsize);
}

PyDoc_STRVAR(endswith__doc__, "");
PyObject *cstring_endswith(PyObject *self, PyObject *args) {
    struct _substr_params params;
    if(!_parse_substr_args(self, args, &params))
        return NULL;
    if(params.end - params.start < params.substr_len)
        return PyBool_FromLong(0);
    int cmp = memcmp(params.end - params.substr_len, params.substr, params.substr_len);
    return PyBool_FromLong(cmp == 0);
}

PyDoc_STRVAR(swapcase__doc__, "");
PyObject *cstring_swapcase(PyObject *self, PyObject *args) {
    struct cstring *new = CSTRING_ALLOC(Py_TYPE(self), Py_SIZE(self));
    if(!new)
        return NULL;
    const char *s = CSTRING_VALUE(self);
    char *d = CSTRING_VALUE(new);

    for(;*s; ++s, ++d) {
        if(islower(*s)) {
            *d = toupper(*s);
        } else if(isupper(*s)) {
            *d = tolower(*s);
        } else {
            *d = *s;
        }
    }

    return (PyObject *)new;
}

PyDoc_STRVAR(upper__doc__, "");
PyObject *cstring_upper(PyObject *self, PyObject *args) {
    struct cstring *new = CSTRING_ALLOC(Py_TYPE(self), Py_SIZE(self));
    if(!new)
        return NULL;
    const char *s = CSTRING_VALUE(self);
    char *d = CSTRING_VALUE(new);

    while((*d++ = toupper(*s++)) != '\0')
        ;

    return (PyObject *)new;
}

static PySequenceMethods cstring_as_sequence = {
    .sq_length = cstring_len,
    .sq_concat = cstring_concat,
    .sq_repeat = cstring_repeat,
    .sq_item = cstring_item,
    .sq_contains = cstring_contains,
};

static PyMappingMethods cstring_as_mapping = {
    .mp_length = cstring_len,
    .mp_subscript = cstring_subscript,
};

static PyMethodDef cstring_methods[] = {
    /* TODO: capitalize */
    /* TODO: casefold */
    /* TODO: center */
    {"count", cstring_count, METH_VARARGS, count__doc__},
    /* TODO: encode (decode???) */
    {"endswith", cstring_endswith, METH_VARARGS, endswith__doc__},
    /* TODO: expandtabs */
    {"find", cstring_find, METH_VARARGS, find__doc__},
    /* TODO: format */
    /* TODO: format_map */
    {"index", cstring_index, METH_VARARGS, index__doc__},
    {"isalnum", cstring_isalnum, METH_NOARGS, isalnum__doc__},
    {"isalpha", cstring_isalpha, METH_NOARGS, isalpha__doc__},
    /* TODO: isascii */
    /* TODO: isdecimal */
    {"isdigit", cstring_isdigit, METH_NOARGS, isdigit__doc__},
    /* TODO: isidentifier */
    {"islower", cstring_islower, METH_NOARGS, islower__doc__},
    /* TODO: isnumeric */
    {"isprintable", cstring_isprintable, METH_NOARGS, isprintable__doc__},
    {"isspace", cstring_isspace, METH_NOARGS, isspace__doc__},
    /* TODO: istitle */
    {"isupper", cstring_isupper, METH_NOARGS, isupper__doc__},
    {"join", cstring_join, METH_O, join__doc__},
    /* TODO: ljust */
    {"lower", cstring_lower, METH_NOARGS, lower__doc__},
    {"lstrip", cstring_lstrip, METH_VARARGS, lstrip__doc__},
    /* TODO: maketrans */
    {"partition", cstring_partition, METH_O, partition__doc__},
    /* TODO: removeprefix */
    /* TODO: replace */
    {"rfind", cstring_rfind, METH_VARARGS, rfind__doc__},
    {"rindex", cstring_rindex, METH_VARARGS, rindex__doc__},
    /* TODO: rjust */
    {"rpartition", cstring_rpartition, METH_O, rpartition__doc__},
    /* TODO: rsplit */
    {"rstrip", cstring_rstrip, METH_VARARGS, rstrip__doc__},
    {"split", (PyCFunction)cstring_split, METH_VARARGS | METH_KEYWORDS, split__doc__},
    /* TODO: splitlines */
    {"startswith", cstring_startswith, METH_VARARGS, startswith__doc__},
    {"strip", cstring_strip, METH_VARARGS, strip__doc__},
    {"swapcase", cstring_swapcase, METH_NOARGS, swapcase__doc__},
    /* TODO: title */
    /* TODO: translate */
    {"upper", cstring_upper, METH_NOARGS, upper__doc__},
    /* TODO: zfill */
    {0},
};

static PyTypeObject cstring_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cstring.cstring",
    .tp_doc = "",
    .tp_basicsize = sizeof(struct cstring),
    .tp_itemsize = sizeof(char),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = cstring_new,
    .tp_dealloc = cstring_dealloc,
    .tp_richcompare = cstring_richcompare,
    .tp_str = cstring_str,
    .tp_repr = cstring_repr,
    .tp_hash = cstring_hash,
    .tp_as_sequence = &cstring_as_sequence,
    .tp_as_mapping = &cstring_as_mapping,
    .tp_methods = cstring_methods,
};

static struct PyModuleDef module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "cstring",
    .m_doc = "",
    .m_size = 0,
    .m_methods = NULL,
};

PyMODINIT_FUNC PyInit_cstring(void) {
    if(PyType_Ready(&cstring_type) < 0)
        return NULL;
    Py_INCREF(&cstring_type);
    PyObject *m = PyModule_Create(&module);
    PyModule_AddObject(m, "cstring", (PyObject *)&cstring_type);
    return m;
}
