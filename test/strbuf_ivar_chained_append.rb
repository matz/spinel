# Each append link on an already shared String must mutate the same handle.
# Mutating the alias first makes the ivar shared before the chain is emitted.

class AppendBox
  def chain
    @s = +"a"
    t = @s
    t << "!"
    r = @s << "x" << "q"
    p @s
    p t
    p r
    r << "?"
    p @s
  end

  def long_chain
    @s = +"a"
    t = @s
    t << "!"
    r = @s << "x" << "q" << "z"
    p @s
    p t
    p r
    r << "?"
    p @s
  end

  def concat
    @s = +"a"
    t = @s
    t << "!"
    r = (@s << "x").concat("q")
    p @s
    p t
    p r
    r << "?"
    p @s
  end

  def concat_chain
    @s = +"a"
    t = @s
    t << "!"
    r = @s.concat("x") << "q"
    p @s
    p t
    p r
    r << "?"
    p @s
  end
  # Calls in the arguments also exercise ordered receiver temporaries.
  def piece(x)
    p x
    x.to_s
  end

  def dynamic_chain
    @s = +"a"
    t = @s
    t << "!"
    r = ((@s << piece(1)).concat(piece(2))) << piece(3)
    p @s
    p t
    p r
    r << "?"
    p @s
  end
end

AppendBox.new.chain
AppendBox.new.long_chain
AppendBox.new.concat
AppendBox.new.concat_chain
AppendBox.new.dynamic_chain

# Locals use the same handle receiver path.
def local_chain
  s = +"a"
  t = s
  t << "!"
  r = s << "x" << "q"
  p s
  p t
  p r
  r << "?"
  p s
end
local_chain

def local_concat
  s = +"a"
  t = s
  t << "!"
  r = (s << "x").concat("q")
  p s
  p t
  p r
  r << "?"
  p s
end
local_concat
