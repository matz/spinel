# A method named define_method that an `each` over literals names, with no
# literal spelling the name whole, runs its block as a block: the block's
# `next` stays a `next`.

class Reg
  [:method].each { |v| define_method("define_#{v}") { |n, &b| [n, b.call] } }

  def run
    v = define_method(:a) { next 1 if ARGV.length == 0; 2 }
    [v, :after]
  end
end
p Reg.new.run

class Tail
  [:define, :other].each { |v| define_method(:"#{v}_method") { |n, &b| [n, b.call] } }

  def run
    v = define_method(:b) { next if ARGV.length == 0; 2 }
    [v, :tail]
  end
end
p Tail.new.run
