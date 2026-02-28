namespace Netc;

/// <summary>
/// Result codes matching netc_result_t from the C API.
/// </summary>
public enum NetcResult : int
{
    Ok          =  0,
    ErrNoMem    = -1,
    ErrTooBig   = -2,
    ErrCorrupt  = -3,
    ErrDictInvalid = -4,
    ErrBufSmall = -5,
    ErrCtxNull  = -6,
    ErrUnsupported = -7,
    ErrVersion  = -8,
    ErrInvalidArg = -9,
}
