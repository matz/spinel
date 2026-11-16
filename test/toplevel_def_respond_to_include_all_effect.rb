# respond_to? of a public top-level def, with an include_all argument that
# has an effect: the argument runs once, after the receiver, whatever the
# answer is; a word the run decides reads its cell after the argument.
def f = 1
def g = 2
public :f
public :g if ARGV.empty?
[1, "s", nil].each { |o| p o.respond_to?(:f, (puts "flag"; false)) }
p 3.respond_to?(:g, (puts("flag g"); false))
p "s".respond_to?(:g, (puts("flag g2"); true))
def recv = (puts "recv"; 4)
p recv.respond_to?(:g, (puts("arg"); false))

# a receiver whose answer is fixed (a String, nil, a program object with the
# method itself) runs the argument too, after the receiver
class K2; def m = 1; end
$l = []
p "s".respond_to?(:f, ($l << :ia1; false)), $l
p nil.respond_to?(:f, ($l << :ia2; false)), $l
p K2.new.respond_to?(:m, ($l << :ia3; false)), $l

# an argument that can raise is run too, though it has no other effect
def probe_s(n) = "s".respond_to?(:f, 1 / n)
def probe_nil(n) = nil.respond_to?(:f, 1 / n)
def probe_obj(n) = K2.new.respond_to?(:m, 1 / n)
[0, 1].each do |n|
  [:probe_s, :probe_nil, :probe_obj].each do |m|
    begin
      p send(m, n)
    rescue ZeroDivisionError => e
      p e.class
    end
  end
end
