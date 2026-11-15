# A String method that changes its receiver reads the receiver once its
# arguments ran, as CRuby does, also when the argument that grows the String
# is the parent of a constant path: `pick::PAT` runs `pick` before it reads
# PAT. The path was counted as a plain constant read, which runs nothing, so
# the receiver's bytes were taken ahead of `pick`, and the method worked on
# the old payload (here: it found no match and answered nil). A path whose
# parent is a constant, as `K::PAT` is, runs nothing and keeps its form.

class K
  PAT = "b"
  PRE = "x"
end

def sm(x) = x.is_a?(String) ? [x.size, x[0, 4]] : x

$s = +"a"
def pick
  $s.replace("b" * 4096)
  K
end
r = $s.sub!(pick::PAT, "x")
p [sm(r), sm($s)]

$s = +"a"
r = $s.gsub!(pick::PAT, "y")
p [r.equal?($s), sm($s)]

$s = +"a"
p [sm($s.delete_prefix!(pick::PRE)), sm($s)]

s = +"a"
pk = -> { s.replace("b" * 4096); K }
p [sm(s.sub!(pk.call::PAT, "w")), sm(s)]

class H
  def initialize = @s = +"a"
  def grow
    @s.replace("b" * 4096)
    K
  end
  def run = [sm(@s.sub!(grow::PAT, "v")), sm(@s)]
end
p H.new.run

t = +"b"
p [sm(t.sub!(K::PAT, "u")), sm(t)]
