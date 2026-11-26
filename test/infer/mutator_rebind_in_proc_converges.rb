# A String mutator on a local a proc assigns, with an argument that may run a
# proc: desugar_mutator_recv_rebind snapshots the receiver into __rr_N, whose
# write lands in the same proc, and the snapshot was rebound again every
# round to the fixpoint's cap.
def label(x)
  x.to_s
end

def render(v)
  run = proc do
    cap = String.new
    cap << label(v)
    cap << "!"
    cap
  end
  run.call
end

puts render(1)
