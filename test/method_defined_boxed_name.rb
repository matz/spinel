# method_defined? and its kin with a name the run time holds boxed (a
# block's variable over an Array of Symbols, a String among them): read
# as a Symbol or a String, anything else is CRuby's TypeError.
class Base
  def inherited_one = 1
  protected def prot = 2
  private def priv = 3
end

class Ticket < Base
  def assign(to) = to
end

[:assign, :missing, :inherited_one, :prot, :priv].each do |name|
  puts "#{name}: #{Ticket.method_defined?(name)} #{Ticket.method_defined?(name, false)} " \
       "#{Ticket.public_method_defined?(name)} #{Ticket.private_method_defined?(name)}"
end

["assign", :assign].each { |n| p Ticket.method_defined?(n) }

[:assign, 1].each do |n|
  p Ticket.method_defined?(n)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end

# instance_method(name).parameters, the same way
[:assign, :inherited_one, "assign"].each do |name|
  puts "#{name}: #{Ticket.instance_method(name).parameters.inspect}"
end
[:missing, 2].each do |name|
  p Ticket.instance_method(name).parameters
rescue NameError, TypeError => e
  puts "#{e.class}: #{e.message}"
end
