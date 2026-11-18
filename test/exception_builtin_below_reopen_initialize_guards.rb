# The initialize a reopening above a builtin exception gives it must not take over
# what the construction does natively:
#   - a class whose own initialize takes arguments of its own (UncaughtThrowError
#     needs two) keeps its arity check for a call with none;
#   - the receiver and key the native arms mark as not recorded stay unrecorded
#     (`KeyError.new.key` raises) until an initialize records one;
#   - a reopened initialize that yields has no plain function to call: the
#     construction runs its proc-form clone on the call's block, so the message
#     goes through it as it does through any other reopened initialize.
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
p ArgumentError.new("a") { 7 }.message
p KeyError.new("k").class
