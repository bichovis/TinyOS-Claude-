/* errno.c - La variable, y como se cuenta cada fallo
 *
 * La define la libc y no el kernel: el kernel devuelve el motivo dentro
 * del valor, y este es el sitio donde se convierte en lo que espera un
 * programa de Unix.
 */
#include "errno.h"

int errno;

/* Los mensajes dicen QUE paso, no "error 17".
 *
 * Un numero obliga a ir a buscarlo a una tabla, y en un sistema pequenyo
 * esa tabla suele estar en la cabeza de quien lo escribio. Aqui estan los
 * treinta bytes que hacen falta para que no haga falta. */
const char *strerror(int codigo)
{
    switch (codigo) {
    case 0:            return "todo bien";
    case EPERM:        return "no te toca a ti";
    case ENOENT:       return "no existe";
    case ESRCH:        return "ese proceso no esta";
    case EINTR:        return "te interrumpio una senyal";
    case EIO:          return "el hardware dijo que no";
    case EBADF:        return "ese descriptor no es tuyo";
    case ECHILD:       return "no tienes hijos que esperar";
    case EAGAIN:       return "ahora no, vuelve a intentarlo";
    case ENOMEM:       return "no hay memoria";
    case EACCES:       return "existe, pero no para eso";
    case EFAULT:       return "ese puntero no vale";
    case EBUSY:        return "esta ocupado";
    case EEXIST:       return "ya existe";
    case EXDEV:        return "eso cruza de volumen";
    case ENODEV:       return "no hay tal dispositivo";
    case ENOTDIR:      return "no es un directorio";
    case EISDIR:       return "es un directorio";
    case EINVAL:       return "eso no tiene sentido";
    case EMFILE:       return "no te quedan descriptores";
    case ENOSPC:       return "no cabe";
    case ESPIPE:       return "eso no se puede rebobinar";
    case EPIPE:        return "al otro lado no hay nadie";
    case ERANGE:       return "el numero no cabe";
    case ENAMETOOLONG: return "ese nombre es demasiado largo";
    case ENOSYS:       return "eso no esta hecho";
    case ENOTEMPTY:    return "el directorio tiene cosas dentro";
    default:           return "no se que ha pasado";
    }
}
