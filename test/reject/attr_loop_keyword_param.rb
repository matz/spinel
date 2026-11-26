# a block with a keyword parameter: the macro's loop is not run at compile time,
# so its attr_* call is refused for its non-literal name
class K
  def self.mk = [:field].each { |n, required: 1| attr_accessor n }
  mk
end
p K.new.respond_to?(:field)
