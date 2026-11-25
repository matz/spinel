# A String mutator answers, and changes, the String its receiver held when
# it was read, ahead of its arguments, though an argument runs code that
# rebinds the variable: a proc that assigns the local, a method that
# assigns the instance variable, any code for a global.
def run
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.sub!(n.call, "K") rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.gsub!(n.call, "K") rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.tr!(n.call, "K") rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.delete!(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.squeeze!(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.replace(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.slice!(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w[n.call] = "K"))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.insert(1, n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.tr_s!(n.call, "K") rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "i" }
  p((w.delete_prefix!(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.delete_suffix!(n.call) rescue $!.class))
  p w
  w = +"ij"
  p((w.setbyte(0, (w = +"TU"; 65)) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "BINARY" }
  p((w.force_encoding(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.sub(n.call, "K") rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.count(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "j" }
  p((w.index(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "i" }
  p((w.start_with?(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "a*" }
  p((w.unpack1(n.call) rescue $!.class))
  p w
  w = +"ij"
  n = proc { w = +"TU"; "*" }
  p((w.center(5, n.call) rescue $!.class))
  p w
end
run
class K
  def initialize = @w = +"ij"
  def swap = (@w = +"TU"; "j")
  def go
    p @w.sub!(swap, "K")
    p @w
    @w = +"ij"
    p @w.insert(1, swap)
    p @w
  end
end
K.new.go
$w = +"ij"
def gswap = ($w = +"TU"; "j")
p $w.delete!(gswap)
p $w
def plain2
  s = +"ab"
  p s.sub!("a", "b".upcase)
  p s
end
plain2
