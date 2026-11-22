# A NoMethodError the runtime raises (`nil.foo`) carries its name and receiver
# also when nothing rescues it and no at_exit hook is armed: a reopened
# #message that reads them in the report gets them, as in CRuby, not nil.
class NoMethodError
  def message
    "name=#{name.inspect} receiver=#{receiver.inspect} args=#{args.inspect}"
  end
end

def missing_on(x) = x.foo(1)

puts "before"
missing_on(nil)
