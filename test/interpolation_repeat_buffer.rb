# frozen_string_literal: true

def render(word, count)
  "#{word * count}!"
end

def render_before_mutation(word, count)
  "#{word * count}" "#{word.clear}"
end

p render("a\0b", 3).bytes
p render("é", 0)
p render("\xff".b, 2).bytes
p render("\xff".b, 2).encoding.name
p render_before_mutation(+"ab", 4)

begin
  render("x", -1)
rescue ArgumentError => error
  puts error.message
end

value = +"a"
value << "#{value * 3}!"
p value
