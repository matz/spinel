# frozen_string_literal: true

def render(word, count)
  "#{word * count}!"
end

def render_before_mutation(word, count)
  "#{word * count}" "#{word.clear}"
end

def render_with_conditional_repeat(flag)
  word = +"a"
  "#{word * 2}#{(if flag; word.replace("R"); word; else; word; end) * 2}"
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

begin
  render(nil, -1)
rescue NoMethodError => error
  p error.name
end

p render_with_conditional_repeat(true)
p render_with_conditional_repeat(false)

value = +"a"
value << "#{value * 3}!"
p value
