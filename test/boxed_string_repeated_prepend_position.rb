module A
  def size = 10*super+1
end
module B
  def size = 10*super+2
end
$n=0
2.times do
  $n+=1
  class String
    prepend A if $n == 2
    p "abc".size
    prepend B
    p "abc".size
    prepend A
    p "abc".size
  end
end
