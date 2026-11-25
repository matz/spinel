# respond_to? of a name known only at run time asks respond_to_missing?
# where no method answers it, as a literal name's does.
class Ghost
  def method_missing(name, *args) = name.to_s.start_with?("boo") ? "mm:#{name}" : super
  def respond_to_missing?(name, priv = false) = name.to_s.start_with?("boo") || (priv && name == :secret) || super
  def real = 1
  def ask(n) = respond_to?(n)
  def ask2(n) = respond_to?(n, true)
end
class Plain; def real = 2; end
g = Ghost.new
names = [:boo, :real, "boo2", :nope, "secret"]
names.each { |n| p [n, g.respond_to?(n), g.respond_to?(n, true), g.ask(n), g.ask2(n)] }
[Ghost.new, Plain.new].each { |o| names.each { |n| p [o.class, n, o.respond_to?(n)] } }
flag = ARGV.empty?
p g.respond_to?("sec" + "ret", flag)
p g.respond_to?("boo#{names.size}".to_sym)
