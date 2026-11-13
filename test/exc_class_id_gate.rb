# A builtin exception answers class questions by its class id: the gated
# accessors (StopIteration#result, NoMethodError#args, KeyError#key ...),
# is_a? and rescue agree with the class tree for builtin instances, program
# subclasses two levels deep, a program class named like a builtin inside a
# module, and an accessor called on a class that does not define it.

require "tmpdir"

def try
  yield
rescue NoMethodError => e
  "NoMethodError #{e.message[/undefined method '\w[\w?]*' for an instance of \S+/]}"
end

# builtin instances, each through its own accessor
si = StopIteration.new("done")
p si.result
ke = begin; {a: 1}.fetch(:b); rescue KeyError => x; x; end
p [ke.key, ke.receiver]
ne = begin; undefined_thing_xyz; rescue NameError => x; x; end
p [ne.name, ne.class]
nm = begin; 3.nope(1, 2); rescue NoMethodError => x; x; end
p [nm.name, nm.args, nm.receiver, nm.private_call?]
fe = begin; "lit".freeze.instance_variable_set(:@a, 1); rescue FrozenError => x; x; end
p [fe.receiver, fe.class]
lj = begin; raise LocalJumpError, "lj"; rescue LocalJumpError => x; x; end
p [lj.reason, lj.exit_value]
ut = begin; throw :nope; rescue UncaughtThrowError => x; x; end
p [ut.tag, ut.value]
se = begin; exit 3; rescue SystemExit => x; x; end
p [se.status, se.success?]
en = begin; File.open(File.join(Dir.tmpdir, "spinel_no_such_dir_zzz", "f")); rescue SystemCallError => x; x; end
p [en.class, en.errno, en.is_a?(SystemCallError)]

# a subclass reaches its ancestors' accessors
class ClosedLike < StopIteration; end
class Deeper < ClosedLike; end
d = Deeper.new("x")
p d.result, d.is_a?(StopIteration), d.is_a?(IndexError), d.is_a?(StandardError), d.is_a?(KeyError)
class MyKey < KeyError; end
class MyKey2 < MyKey; end
mk = MyKey2.new("k", receiver: {z: 1}, key: :zz)
p [mk.key, mk.receiver, mk.is_a?(IndexError), mk.is_a?(NameError)]
class MyNoMeth < NoMethodError; end
p MyNoMeth.new("m", :foo).name
p ClosedQueueError.new("q").is_a?(StopIteration), ClosedQueueError.new("q").result

# a program class named like a builtin, inside a module, is not the builtin
module Lib
  class StopIteration < StandardError; end
  class KeyError < StandardError; end
end
ls = Lib::StopIteration.new("lib")
p ls.is_a?(::StopIteration), ls.is_a?(StandardError), ls.class
p try { ls.result }
lk = Lib::KeyError.new("lib")
p lk.is_a?(::KeyError), lk.is_a?(::IndexError)
p try { lk.key }

# the accessor called on a class that does not define it
re = RuntimeError.new("rt")
p try { re.result }
p try { re.args }
p try { re.private_call? }
p try { re.reason }
p try { re.tag }
p try { re.status }
p try { re.signo }
p try { StandardError.new("s").name }
p try { ArgumentError.new("a").key }
p try { IndexError.new("i").receiver }
p try { StopIteration.new("s").key }
p try { KeyError.new("k").result }

# is_a? and rescue over the builtin tree, by class
tree = [StopIteration, IndexError, KeyError, NameError, NoMethodError, FrozenError, RuntimeError,
        StandardError, Exception, ArgumentError, UncaughtThrowError, SystemExit, ScriptError,
        NotImplementedError, LoadError, ZeroDivisionError, FloatDomainError, RangeError,
        Interrupt, SignalException, EOFError, IOError, ClosedQueueError, LocalJumpError]
tree.each do |a|
  inst = a == UncaughtThrowError ? UncaughtThrowError.new(:t, 1) : a == SignalException ? SignalException.new("INT") : a.new
  ups = tree.select { |b| inst.is_a?(b) }
  puts "#{a}: #{ups.join(' ')}"
end
def rescued(e)
  begin
    raise e
  rescue KeyError then :key
  rescue IndexError then :index
  rescue NameError then :name
  rescue RuntimeError then :runtime
  rescue ArgumentError then :arg
  rescue StandardError then :std
  rescue ScriptError then :script
  rescue Exception then :exc
  end
end
p [StopIteration, KeyError, NoMethodError, FrozenError, FloatDomainError, NotImplementedError,
   ClosedQueueError, SystemExit, ZeroDivisionError, Deeper, MyKey2].map { |c| rescued(c.new) }

# a boxed exception through a poly slot
xs = [3, StopIteration.new("a"), :sym, KeyError.new("b"), Deeper.new("c")]
xs.each { |o| p [o.class, o.respond_to?(:result), (o.is_a?(StopIteration) ? o.result : :no)] }
