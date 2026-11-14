# compact and compact! on a typed Hash slot that may hold nil raise
# NoMethodError under their own names, as CRuby's nil does, before the
# Hash is read; safe navigation answers nil.
class K
  def fill
    @h = {1 => 2}
    @s = {"a" => +"x"}
    @n = {"b" => nil}
  end
  def try
    [-> { @h.compact }, -> { @h.compact! }, -> { @s.compact }, -> { @s.compact! },
     -> { @n.compact }, -> { @n.compact! }].each do |f|
      p f.call
    rescue NoMethodError => e
      p [e.class, e.name]
    end
    p @h&.compact, @s&.compact
  end
end
k = K.new
k.try
k.fill
k.try

def mk(i) = i.odd? ? {1 => 2} : nil
h = mk(ARGV.size)
begin
  h.compact
rescue NoMethodError => e
  p [e.class, e.name]
end
p mk(1).compact, mk(1).compact!
