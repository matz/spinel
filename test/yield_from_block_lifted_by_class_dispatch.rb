# A yield (and block_given?) inside a block passed to a method on a class
# chosen at run time: that call takes the block as a proc, and the yield
# still reaches the enclosing method's block.
class Wav
  def self.open(path) = yield(new)
end

class Raw
  def self.open(path) = yield(new)
end

class Renderer
  def render(kind)
    container = kind == 1 ? Wav : Raw
    container.open("x") do |w|
      yield w.class if block_given?
    end
  end

  def each_container
    [Wav, Raw].each { |k| k.open("y") { |w| yield w.class } }
  end
end

Renderer.new.render(1) { |c| p c }
Renderer.new.render(2) { |c| p c }
p Renderer.new.render(1)
Renderer.new.each_container { |c| p [:each, c] }

# The same through a poly receiver: the union holds Classes, whose class
# method takes the block, and an object whose instance method does not.
class Drive
  def open(path) = path
end

class Player
  def pick(kind) = kind == 1 ? Wav : (kind == 2 ? Raw : Drive.new)

  def play(kind)
    pick(kind).open("z") { |w| yield [:play, w.class] }
  end
end

Player.new.play(1) { |c| p c }
Player.new.play(2) { |c| p c }
p Player.new.play(3) { |c| p c }
