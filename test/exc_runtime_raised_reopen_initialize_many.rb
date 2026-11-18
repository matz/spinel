# More than eight reopenings of builtin exceptions that define an initialize: the
# one the runtime's error reaches is picked by its class's ancestry, whichever
# place it has in the file (ZeroDivisionError is the eleventh here).
class ArgumentError; def initialize(m = nil) = super("ArgumentError<" + m.to_s + ">"); end
class TypeError; def initialize(m = nil) = super("TypeError<" + m.to_s + ">"); end
class RangeError; def initialize(m = nil) = super("RangeError<" + m.to_s + ">"); end
class IOError; def initialize(m = nil) = super("IOError<" + m.to_s + ">"); end
class EOFError; def initialize(m = nil) = super("EOFError<" + m.to_s + ">"); end
class RegexpError; def initialize(m = nil) = super("RegexpError<" + m.to_s + ">"); end
class FiberError; def initialize(m = nil) = super("FiberError<" + m.to_s + ">"); end
class ThreadError; def initialize(m = nil) = super("ThreadError<" + m.to_s + ">"); end
class StopIteration; def initialize(m = nil) = super("StopIteration<" + m.to_s + ">"); end
class IndexError; def initialize(m = nil) = super("IndexError<" + m.to_s + ">"); end
class ZeroDivisionError; def initialize(m = nil) = super("ZeroDivisionError<" + m.to_s + ">"); end
def t; yield; rescue => e; puts "#{e.class} #{e.message}"; end
t { 1 / 0 }
t { Integer("z") }
t { [1].fetch(9) }
t { [1, 2].first(-1) }
