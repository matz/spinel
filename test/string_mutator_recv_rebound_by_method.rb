# A String mutator's receiver is an ivar or a global that a method the
# argument calls assigns: the mutation goes to the String the receiver held
# when it was read, as CRuby evaluates the receiver first.
def reas(k) = ($t << k; @s = +"WXYZ"; +"qrs")
def reas2(k) = ($t << k; $s = +"WXYZ"; +"qrs")
def mut_i(k) = ($t << k; @s << "z"; 1)

def app
  $t = []; @s = +"ab"; r = (@s << reas(2)).inspect; p [r, @s, $t]
end
def aset
  $t = []; @s = +"ab"; r = (@s[mut_i(2)] = reas(3)).inspect; p [r, @s, $t]
end
def cat
  $t = []; @s = +"ab"; r = @s.concat(reas(2)).inspect; p [r, @s]
end
def ins
  $t = []; @s = +"ab"; r = @s.insert(1, reas(2)).inspect; p [r, @s]
end
def gl
  $t = []; $s = +"ab"; r = ($s << reas2(2)).inspect; p [r, $s]
end
app; aset; cat; ins; gl
