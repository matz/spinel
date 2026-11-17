# The initialize a reopening above a builtin exception gives it must not take over
# what the construction does natively:
#   - a class whose own initialize takes arguments of its own (UncaughtThrowError
#     needs two) keeps its arity check for a call with none;
#   - the receiver and key the native arms mark as not recorded stay unrecorded
#     (`KeyError.new.key` raises) until an initialize records one;
#   - a reopened initialize that yields has no plain function to call: the
#     construction keeps the native path rather than naming a function that is
#     not emitted. (Only the class of the result is checked: the message does not
#     yet go through such an initialize.)
class StandardError
  def initialize(msg = nil)
    yield if block_given?
    super("S[#{msg}]")
  end
end
begin; UncaughtThrowError.new; p :built; rescue ArgumentError => e; p e.class; end
begin; KeyError.new.key; p :no_raise; rescue => e; p e.class; end
begin; NameError.new.receiver; p :no_raise; rescue => e; p e.class; end
begin; FrozenError.new("f").receiver; p :no_raise; rescue => e; p e.class; end
p ArgumentError.new("a") { 7 }.class
p KeyError.new("k").class
