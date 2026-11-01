# The `...` forwarding rewrite gives the answer CRuby gives. The program is
# also compiled after a long flat prefix under a small stack, by
# forwarding-large-ast-test (tools/forwarding_large_ast_test.sh).
def leaf(*values)
  values.sum
end

def forward(...)
  leaf(...)
end

puts forward(20, 22)
