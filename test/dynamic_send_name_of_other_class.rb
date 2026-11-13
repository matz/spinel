# A computed-name send to an instance whose class has no method of the name,
# where the name is a class method or another class's method that always
# raises or throws, raises CRuby's NoMethodError with the receiver and the name.
class A
  def self.terminate(x = nil) = throw(:terminate, x)
  def self.cboom = raise("cboom")
  def other = 1
end
class B
  def m = 2
  def boom = raise("boom")
  def arg_boom(x) = raise("ab #{x}")
end
def show(o, n, *a)
  begin
    p o.send(n, *a)
  rescue NoMethodError => e
    puts "#{e.class}: #{e.message} recv=#{e.receiver.class} name=#{e.name.inspect}"
  rescue => e
    puts "#{e.class}: #{e.message}"
  end
end
objs = [A.new, B.new]
%w[terminate cboom m other boom arg_boom].each do |n|
  objs.each { |o| show(o, n) }
end
a = A.new
%w[terminate cboom boom m other].each { |n| show(a, n) }
%w[terminate cboom boom m other].each { |n| begin; p a.public_send(n); rescue NoMethodError => e; puts "#{e.message} #{e.receiver.class} #{e.name.inspect}"; rescue => e; puts e.message; end }
b = B.new
%w[terminate cboom boom m other arg_boom].each { |n| show(b, n, 1) }
