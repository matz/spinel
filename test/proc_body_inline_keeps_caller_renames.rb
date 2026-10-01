def twice
  total = 2
  yield total
end

def render(path)
  pr = proc { twice { |n| p n } }
  pr.call
  yield path
end

render("out.wav") { |q| p q }
