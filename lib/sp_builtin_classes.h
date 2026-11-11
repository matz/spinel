/* The builtin classes' names and class ids, one X row each (key, name, id,
   BC_* flags): the one source of the table the compiler reads
   (builtin_names.c) and the ids the generated program carries in a builtin's
   class value (sp_Class.cls_id, SP_BCLS_<key>, sp_builtin_class_id_of). The
   flags only mean something to the compiler. */
#ifndef SP_BUILTIN_CLASSES_H
#define SP_BUILTIN_CLASSES_H
#include <string.h>

#define SP_BUILTIN_CLASSES(X) \
  X(INTEGER, "Integer", -100, BC_CLASS) \
  X(FLOAT, "Float", -101, BC_CLASS) \
  X(STRING, "String", -102, BC_CLASS) \
  X(SYMBOL, "Symbol", -103, BC_CLASS) \
  X(ARRAY, "Array", -104, BC_CLASS) \
  X(HASH, "Hash", -105, BC_CLASS) \
  X(RANGE, "Range", -106, BC_CLASS) \
  X(TIME, "Time", -107, BC_CLASS) \
  X(MODULE, "Module", -108, BC_CLASS) \
  X(CLASS, "Class", -109, BC_CLASS) \
  X(NILCLASS, "NilClass", -110, BC_CLASS) \
  X(TRUECLASS, "TrueClass", -111, BC_CLASS) \
  X(FALSECLASS, "FalseClass", -112, BC_CLASS) \
  X(NUMERIC, "Numeric", -113, BC_CLASS) \
  X(COMPARABLE, "Comparable", -114, BC_CLASS | BC_MODULE) \
  X(ENUMERABLE, "Enumerable", -115, BC_CLASS | BC_MODULE) \
  X(OBJECT, "Object", -116, BC_CLASS | BC_PLAIN) \
  X(BASIC_OBJECT, "BasicObject", -117, BC_CLASS | BC_PLAIN) \
  X(PROC, "Proc", -118, BC_CLASS) \
  X(KERNEL, "Kernel", -119, BC_CLASS | BC_MODULE | BC_SG_BOOT) \
  X(IO, "IO", -120, BC_CLASS) \
  X(FILE, "File", -121, BC_CLASS) \
  X(EXCEPTION, "Exception", -122, BC_CLASS | BC_EXCEPTION) \
  X(STANDARDERROR, "StandardError", -123, BC_CLASS | BC_EXCEPTION) \
  X(RUNTIMEERROR, "RuntimeError", -124, BC_CLASS | BC_EXCEPTION) \
  X(TYPEERROR, "TypeError", -125, BC_CLASS | BC_EXCEPTION) \
  X(ARGUMENTERROR, "ArgumentError", -126, BC_CLASS | BC_EXCEPTION) \
  X(NAMEERROR, "NameError", -127, BC_CLASS | BC_EXCEPTION) \
  X(NOMETHODERROR, "NoMethodError", -128, BC_CLASS | BC_EXCEPTION) \
  X(STOPITERATION, "StopIteration", -129, BC_CLASS | BC_EXCEPTION) \
  X(MATH, "Math", -130, BC_CLASS | BC_MODULE | BC_SG_BOOT) \
  X(COMPLEX, "Complex", -131, BC_CLASS) \
  X(INDEXERROR, "IndexError", -132, BC_CLASS | BC_EXCEPTION) \
  X(KEYERROR, "KeyError", -133, BC_CLASS | BC_EXCEPTION) \
  X(RANGEERROR, "RangeError", -134, BC_CLASS | BC_EXCEPTION) \
  X(FLOATDOMAINERROR, "FloatDomainError", -135, BC_CLASS | BC_EXCEPTION) \
  X(ZERODIVISIONERROR, "ZeroDivisionError", -136, BC_CLASS | BC_EXCEPTION) \
  X(FROZENERROR, "FrozenError", -137, BC_CLASS | BC_EXCEPTION) \
  X(IOERROR, "IOError", -138, BC_CLASS | BC_EXCEPTION) \
  X(LOCALJUMPERROR, "LocalJumpError", -139, BC_CLASS | BC_EXCEPTION) \
  X(NOTIMPLEMENTEDERROR, "NotImplementedError", -140, BC_CLASS | BC_EXCEPTION) \
  X(SCRIPTERROR, "ScriptError", -141, BC_CLASS | BC_EXCEPTION) \
  X(RATIONAL, "Rational", -142, BC_CLASS) \
  X(REGEXP, "Regexp", -143, BC_CLASS) \
  X(ENUMERATOR, "Enumerator", -144, BC_CLASS) \
  X(STRUCT, "Struct", -145, BC_CLASS) \
  X(DATA, "Data", -146, BC_CLASS | BC_PLAIN) \
  X(SYNTAXERROR, "SyntaxError", -147, BC_CLASS | BC_EXCEPTION) \
  X(SECURITYERROR, "SecurityError", -148, BC_CLASS | BC_EXCEPTION) \
  X(REGEXPERROR, "RegexpError", -149, BC_CLASS | BC_EXCEPTION) \
  X(ENCODINGERROR, "EncodingError", -150, BC_CLASS | BC_EXCEPTION) \
  X(SIGNALEXCEPTION, "SignalException", -151, BC_CLASS | BC_EXCEPTION) \
  X(INTERRUPT, "Interrupt", -152, BC_CLASS | BC_EXCEPTION) \
  X(THREADERROR, "ThreadError", -153, BC_CLASS | BC_EXCEPTION) \
  X(FIBERERROR, "FiberError", -154, BC_CLASS | BC_EXCEPTION) \
  X(CLOSEDQUEUEERROR, "ClosedQueueError", -155, BC_CLASS | BC_EXCEPTION) \
  X(UNCAUGHTTHROWERROR, "UncaughtThrowError", -156, BC_CLASS | BC_EXCEPTION) \
  X(NOMATCHINGPATTERNERROR, "NoMatchingPatternError", -157, BC_CLASS | BC_EXCEPTION) \
  X(NOMATCHINGPATTERNKEYERROR, "NoMatchingPatternKeyError", -158, BC_CLASS | BC_EXCEPTION) \
  X(EOFERROR, "EOFError", -159, BC_CLASS | BC_EXCEPTION) \
  X(MATH_DOMAINERROR, "Math::DomainError", -160, BC_EXCEPTION) \
  X(SYSTEMEXIT, "SystemExit", -161, BC_CLASS | BC_EXCEPTION) \
  X(SIGNAL, "Signal", -162, BC_CLASS | BC_MODULE | BC_SG_BOOT) \
  X(PROCESS_STATUS, "Process::Status", -163, BC_CLASS) \
  X(PROCESS_TMS, "Process::Tms", -164, 0) \
  X(DIR, "Dir", -165, BC_CLASS) \
  X(BASICSOCKET, "BasicSocket", -166, BC_CLASS | BC_SOCKET) \
  X(IPSOCKET, "IPSocket", -167, BC_CLASS | BC_SOCKET) \
  X(TCPSOCKET, "TCPSocket", -168, BC_CLASS | BC_SOCKET) \
  X(TCPSERVER, "TCPServer", -169, BC_CLASS | BC_SOCKET) \
  X(UDPSOCKET, "UDPSocket", -170, BC_CLASS | BC_SOCKET) \
  X(UNIXSOCKET, "UNIXSocket", -171, BC_CLASS | BC_SOCKET) \
  X(UNIXSERVER, "UNIXServer", -172, BC_CLASS | BC_SOCKET) \
  X(SOCKET, "Socket", -173, BC_CLASS | BC_SOCKET) \
  X(THREAD, "Thread", -174, BC_CLASS) \
  X(MUTEX, "Mutex", -175, BC_CLASS) \
  X(THREAD_MUTEX, "Thread::Mutex", -175, 0) \
  X(QUEUE, "Queue", -176, BC_CLASS) \
  X(THREAD_QUEUE, "Thread::Queue", -176, 0) \
  X(SIZEDQUEUE, "SizedQueue", -177, BC_CLASS) \
  X(THREAD_SIZEDQUEUE, "Thread::SizedQueue", -177, 0) \
  X(CONDITIONVARIABLE, "ConditionVariable", -178, BC_CLASS) \
  X(THREAD_CONDITIONVARIABLE, "Thread::ConditionVariable", -178, 0) \
  X(FIBER, "Fiber", -179, BC_CLASS) \
  X(MATCHDATA, "MatchData", -180, BC_CLASS) \
  X(ENCODING, "Encoding", 0, BC_CLASS) \
  X(ENUMERATOR_CHAIN, "Enumerator::Chain", 0, BC_CLASS) \
  X(ENUMERATOR_LAZY, "Enumerator::Lazy", 0, BC_CLASS) \
  X(ENUMERATOR_PRODUCT, "Enumerator::Product", 0, BC_CLASS) \
  /* the json package's errors, which lib/sp_exc.c gives their parents (JSONError < StandardError): \
     a known name makes `rescue JSON::JSONError` climb that hierarchy, not compare a name (#7797) */ \
  X(JSON_JSONERROR, "JSON::JSONError", 0, BC_EXCEPTION) \
  X(JSON_PARSERERROR, "JSON::ParserError", 0, BC_EXCEPTION) \
  X(JSON_NESTINGERROR, "JSON::NestingError", 0, BC_EXCEPTION) \
  X(JSON_GENERATORERROR, "JSON::GeneratorError", 0, BC_EXCEPTION) \
  X(ENCODING_COMPATIBILITYERROR, "Encoding::CompatibilityError", 0, BC_EXCEPTION) \
  X(ENCODING_CONVERTERNOTFOUNDERROR, "Encoding::ConverterNotFoundError", 0, BC_EXCEPTION) \
  X(ENCODING_INVALIDBYTESEQUENCEERROR, "Encoding::InvalidByteSequenceError", 0, BC_EXCEPTION) \
  X(ENCODING_UNDEFINEDCONVERSIONERROR, "Encoding::UndefinedConversionError", 0, BC_EXCEPTION) \
  X(ERRNO_ENOENT, "Errno::ENOENT", 0, BC_EXCEPTION) \
  X(GC, "GC", 0, BC_CLASS | BC_MODULE | BC_SG_BOOT) \
  X(IO_EAGAINWAITREADABLE, "IO::EAGAINWaitReadable", 0, BC_EXCEPTION) \
  X(IO_TIMEOUTERROR, "IO::TimeoutError", 0, BC_EXCEPTION) \
  /* the socket library's: lib/sp_exc.c gives them their parents, so \
     `rescue SocketError` catches a failed lookup's Socket::ResolutionError */ \
  X(SOCKETERROR, "SocketError", 0, BC_EXCEPTION) \
  X(SOCKET_RESOLUTIONERROR, "Socket::ResolutionError", 0, BC_EXCEPTION) \
  X(IO_EAGAINWAITWRITABLE, "IO::EAGAINWaitWritable", 0, BC_EXCEPTION) \
  X(IO_EINPROGRESSWAITREADABLE, "IO::EINPROGRESSWaitReadable", 0, BC_EXCEPTION) \
  X(IO_EINPROGRESSWAITWRITABLE, "IO::EINPROGRESSWaitWritable", 0, BC_EXCEPTION) \
  X(IO_EWOULDBLOCKWAITREADABLE, "IO::EWOULDBLOCKWaitReadable", 0, BC_EXCEPTION) \
  X(IO_EWOULDBLOCKWAITWRITABLE, "IO::EWOULDBLOCKWaitWritable", 0, BC_EXCEPTION) \
  X(IO_WAITREADABLE, "IO::WaitReadable", 0, BC_EXCEPTION) \
  X(IO_WAITWRITABLE, "IO::WaitWritable", 0, BC_EXCEPTION) \
  X(LOADERROR, "LoadError", 0, BC_EXCEPTION) \
  X(MARSHAL, "Marshal", 0, BC_CLASS | BC_MODULE | BC_SG_BOOT) \
  X(METHOD, "Method", 0, BC_CLASS) \
  X(NOMEMORYERROR, "NoMemoryError", 0, BC_EXCEPTION) \
  X(OBJECTSPACE, "ObjectSpace", 0, BC_CLASS | BC_MODULE | BC_SG_BOOT) \
  X(OPENSTRUCT, "OpenStruct", 0, BC_CLASS | BC_OSTRUCT) \
  X(PROCESS, "Process", 0, BC_CLASS | BC_MODULE | BC_SG_BOOT) \
  X(RANDOM, "Random", 0, BC_CLASS) \
  X(STRINGSCANNER_ERROR, "StringScanner::Error", 0, BC_EXCEPTION) \
  X(SYSTEMCALLERROR, "SystemCallError", 0, BC_EXCEPTION) \
  X(SYSTEMSTACKERROR, "SystemStackError", 0, BC_EXCEPTION) \
  X(UNBOUNDMETHOD, "UnboundMethod", 0, BC_CLASS)

enum {
#define X(k, n, id, flags) SP_BCLS_##k = (id) ? (id) : -1,   /* (-1: no id, no class has it) */
  SP_BUILTIN_CLASSES(X)
#undef X
  SP_BCLS_END_
};
/* The class id of builtin class `n`, -1 for a name that has none (a row of
   id 0, or no row). The id of a builtin's class value, which the class-value
   switches (a new, a class method) arm on. */
static inline int sp_builtin_class_id_of(const char *n) {
#define X(k, nm, id, flags) if ((id) != 0 && n[0] == (nm)[0] && !strcmp(n, nm)) return (id);
  SP_BUILTIN_CLASSES(X)
#undef X
  return -1;
}
#endif
