# DATA read in the receiver of a constant-path write is the data section
module M; end
def holder(io)
  p io.read
  M
end
holder(DATA)::X = 1
p M::X
__END__
receiver
