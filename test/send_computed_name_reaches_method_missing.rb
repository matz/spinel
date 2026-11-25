# A send of a name known only at run time reaches method_missing where no
# method answers it, as a literal name's does, and raises NoMethodError
# naming the name on a receiver whose class has none.
class Ghost
  def method_missing(name, *args, &blk)
    if name.to_s.start_with?("boo")
      "mm:#{name}:#{args.inspect}:#{blk ? blk.call : '-'}"
    else
      super
    end
  end
  def respond_to_missing?(name, priv = false) = name.to_s.start_with?("boo") || super
  def real = "real"
end
class Plain; def real = "plain"; end
class Priv
  private def method_missing(name, *args) = "priv:#{name}"
end
g = Ghost.new
names = [:boo, :real, "boo2"]
names.each { |n| p g.send(n) }
p g.send(names[0], 1, 2) { 3 }
p g.public_send("boo#{names.size}".to_sym, :x)
begin
  g.send(names.size > 2 ? :zap : :real)
rescue NoMethodError => e
  p e.message
end
objs = [Ghost.new, Plain.new]
objs.each do |o|
  begin
    p o.send(names[0])
  rescue NoMethodError => e
    p e.message
  end
  p o.send(names[1])
end
p Priv.new.send("x#{1}".to_sym)
nm = ARGV.empty? ? :boo : :real
p g.__send__(nm)
