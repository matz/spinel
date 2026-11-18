# remove_const answers the removed constant's value, so the lambda it held
# may run without a read of its name: it starts where its write runs.
module M
  L = -> { p :remove_const }
  remove_const(:L).call
end
