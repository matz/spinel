$events = []
$word = +"start"

def nullable_word(flag)
  $events << :receiver
  GC.start
  flag ? $word : nil
end

def append_operand
  $events << :argument
  $word.replace("current")
  GC.start
  "!"
end

def append_to_word(flag)
  result = nullable_word(flag)&.+@&.concat(append_operand)
  p [result, result.equal?($word), $word, $events]
end

append_to_word(false)
append_to_word(true)

result = nullable_word(true)&.+@&.concat("?")
p [result.equal?($word), result]
