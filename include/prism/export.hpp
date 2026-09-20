#pragma once

#ifdef _WIN32
#  ifdef PRISM_EXPORTS
#    define PRISM_API __declspec(dllexport)
#  else
#    define PRISM_API
#  endif
#else
#  define PRISM_API
#endif
