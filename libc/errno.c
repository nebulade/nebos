/* Global errno and strerror for single-threaded ring-3 commands. */

#include <errno.h>

int errno;

const char *strerror(int err)
{
	switch (err) {
	case 0:            return "Success";
	case EPERM:        return "Operation not permitted";
	case ENOENT:       return "No such file or directory";
	case ESRCH:        return "No such process";
	case EINTR:        return "Interrupted system call";
	case EIO:          return "I/O error";
	case ENXIO:        return "No such device or address";
	case E2BIG:        return "Argument list too long";
	case ENOEXEC:      return "Exec format error";
	case EBADF:        return "Bad file descriptor";
	case ECHILD:       return "No child processes";
	case EAGAIN:       return "Resource temporarily unavailable";
	case ENOMEM:       return "Cannot allocate memory";
	case EACCES:       return "Permission denied";
	case EFAULT:       return "Bad address";
	case EBUSY:        return "Device or resource busy";
	case EEXIST:       return "File exists";
	case EXDEV:        return "Invalid cross-device link";
	case ENODEV:       return "No such device";
	case ENOTDIR:      return "Not a directory";
	case EISDIR:       return "Is a directory";
	case EINVAL:       return "Invalid argument";
	case ENFILE:       return "Too many open files in system";
	case EMFILE:       return "Too many open files";
	case ENOTTY:       return "Inappropriate ioctl for device";
	case EFBIG:        return "File too large";
	case ENOSPC:       return "No space left on device";
	case ESPIPE:       return "Illegal seek";
	case EROFS:        return "Read-only file system";
	case EPIPE:        return "Broken pipe";
	case EDOM:         return "Numerical argument out of domain";
	case ERANGE:       return "Numerical result out of range";
	case ENOSYS:       return "Function not implemented";
	case EOVERFLOW:    return "Value too large for defined data type";
	case EOPNOTSUPP:   return "Operation not supported";
	case ENAMETOOLONG: return "File name too long";
	default:           return "Unknown error";
	}
}
