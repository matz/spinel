# `x&.dup` where x is a value read out of a Hash, in a program that sets an
# instance variable on a builtin value (so a copy carries those over): the
# guard tests and copies x, not a value the method held just before it.

ACRONYMS = { "html" => "HTML" }

def acronyms = ACRONYMS

def camelize(term)
  string = term.to_s
  if string.match?(/\A[a-z\d]*\z/)
    return acronyms[string]&.dup || string.capitalize
  end
  string
end

def first_tag(list)
  list.first&.dup
end

h = {}
h.instance_variable_set(:@tag, 1)
p h.dup.instance_variable_get(:@tag)

p camelize("hello")
p camelize(:world)
p camelize("html")
p camelize("a_b")
p first_tag([h])&.instance_variable_get(:@tag)
p first_tag([])
