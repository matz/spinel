$word = +"a"

class NilClass
  def word = $word
end

class Integer
  def word = +"number"
end

def value(flag)
  receiver = flag ? 8 : nil
  receiver.word
end

answer = value(false)
p answer.equal?($word)
answer << "!"
p $word, value(true)
