# frozen_string_literal: true

class String
  private :*
end

def render(word, count)
  "text:#{word * count}"
rescue NoMethodError => error
  error.name
end

p render("ab", 2)
p render("ab", -1)
