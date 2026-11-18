# method_capture_boxed_arg_chain.rb over twenty-four methods, the last of
# which stores the value: the instance variable, the reader's return and
# each local on the way back follow the widened parameter.
class Store
  def initialize = @value = 0
  def put(value) = @value = value
  def get = @value
end
STORE = Store.new
def hop0(value) = STORE.put(value)
def hop1(value)
  answer = hop0(value)
  answer
end
def hop2(value)
  answer = hop1(value)
  answer
end
def hop3(value)
  answer = hop2(value)
  answer
end
def hop4(value)
  answer = hop3(value)
  answer
end
def hop5(value)
  answer = hop4(value)
  answer
end
def hop6(value)
  answer = hop5(value)
  answer
end
def hop7(value)
  answer = hop6(value)
  answer
end
def hop8(value)
  answer = hop7(value)
  answer
end
def hop9(value)
  answer = hop8(value)
  answer
end
def hop10(value)
  answer = hop9(value)
  answer
end
def hop11(value)
  answer = hop10(value)
  answer
end
def hop12(value)
  answer = hop11(value)
  answer
end
def hop13(value)
  answer = hop12(value)
  answer
end
def hop14(value)
  answer = hop13(value)
  answer
end
def hop15(value)
  answer = hop14(value)
  answer
end
def hop16(value)
  answer = hop15(value)
  answer
end
def hop17(value)
  answer = hop16(value)
  answer
end
def hop18(value)
  answer = hop17(value)
  answer
end
def hop19(value)
  answer = hop18(value)
  answer
end
def hop20(value)
  answer = hop19(value)
  answer
end
def hop21(value)
  answer = hop20(value)
  answer
end
def hop22(value)
  answer = hop21(value)
  answer
end
def hop23(value)
  answer = hop22(value)
  answer
end
def hop24(value)
  answer = hop23(value)
  answer
end
def adapter(box)
  callback = box[:load]
  proc { |value| callback.call(value) }
end
p hop24(0)
f = adapter({load: method(:hop24)})
p f.call(1.5)
p STORE.get
p f.call("s")
p STORE.get
p f.call(nil)
p hop24(2) + 1
p STORE.get
