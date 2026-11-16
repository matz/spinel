# A gated accessor's name the program defines on Object (every exception
# answers it) or in a reopening of a builtin exception (that class and its
# subclasses answer it); the other classes still answer by their own gate.
class Object; def tag = :obj; end
class StandardError; def key = :own; end
p RuntimeError.new.respond_to?(:tag), UncaughtThrowError.new(:t, 1).respond_to?(:tag), NameError.new("m").respond_to?(:tag)
p StandardError.new.respond_to?(:key), RuntimeError.new.respond_to?(:key), ScriptError.new.respond_to?(:key)
p NameError.new("m").respond_to?(:args), KeyError.new("m").respond_to?(:key)
