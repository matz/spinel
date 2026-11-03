# A proc that hands its String parameter on to a method that only reads it
# (a pure reader's receiver, printed, interpolated) does not append to it.
# Every String parameter is the shared handle, and the hand-on counted that
# as an append: a proc called with a String that cannot be the handle (a
# block's parameter of a Hash iterator, an instance variable) was refused.
class Registry
  def initialize
    @seen = { "app" => true, "app/a" => true }
    @kept = { "app/k" => true }
    @instances = { "app" => 1, "app/a" => 2, "app/b" => 3, "app/k/x" => 4, "other" => 5 }
  end

  def inside_kept?(id)
    return false if @kept.empty?

    cut = id.rindex("/")
    while cut
      return true if @kept.key?(id[0, cut].to_s)

      cut = cut.zero? ? nil : id.rindex("/", cut - 1)
    end
    false
  end

  def forget(scope)
    under = scope && "#{scope}/"
    keep = ->(id) { @seen.key?(id) || (!under.nil? && !id.start_with?(under)) || inside_kept?(id) }
    @instances.select! { |id, _| keep.call(id) }
    @instances.keys
  end
end

p Registry.new.forget("app")
p Registry.new.forget(nil)

# an instance variable handed to a lambda that hands it on to a reader,
# a printer and an interpolation; the caller's own appends still show
class Reader
  def initialize = (@s = +"ab")
  def width(s) = s.size
  def show(s) = (puts s; "<#{s}>")
  def run
    f = ->(s) { [width(s), show(s)] }
    p f.call(@s)
    @s << "c"
    p f.call(@s), @s
  end
end
Reader.new.run

# a Hash key handed to a reader through a proc, by each and by select
h = { "x/y" => 1, "z" => 2 }
def slash?(k) = k.include?("/")
pr = proc { |k| slash?(k) }
h.each { |k, _| p pr.call(k) }
p h.select { |k, _| pr.call(k) }.keys

# a Method of a reader, read out of an instance variable and called through
# its proc with an instance variable and a Hash key; the reader is a block
# argument too (&method), which makes its parameter the handle
class Lookup
  def initialize
    @rd = method(:slash?)
    @s = +"q/r"
  end
  def slash?(k) = k.include?("/")
  def run
    p ["a/b", "c"].select(&method(:slash?))
    g = @rd.to_proc
    p g.call(@s), @rd.call(@s)
    h = { "x/y" => 1, "z" => 2 }
    p h.select { |k, _| g.call(k) }.keys
  end
end
Lookup.new.run
