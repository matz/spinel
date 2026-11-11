# A seed must not change what the program DOES. The output-buffer pattern
# (`def render_into(io, ...); io << ...; end`) relies on the caller seeing
# the callee's appends, which spinel gives it by passing String params as the
# caller's handle. A truthful `(String io)` seed used to disqualify the
# parameter from sharing, so every append landed in a copy and the caller
# got an empty buffer -- silently, and only when a sidecar was present.
# spinel: rbs-seed-check
module Views
  module Parts
    def self.frag_into(io, n)
      io << "part#{n};"
      nil
    end
  end
end

# Forwards its buffer and never appends to it itself, so its parameter can
# only be shared through the TRANSITIVE arm -- the second site the seed
# exclusion sat on.
module Views
  module Body
    def self.frags_into(io)
      Views::Parts.frag_into(io, 1)
      Views::Parts.frag_into(io, 2)
      nil
    end
  end
end

module Views
  module Page
    def self.show_into(io)
      io << "<html>"
      Views::Body.frags_into(io)
      io << "</html>"
      nil
    end

    def self.show
      io = String.new
      Views::Page.show_into(io)
      io
    end
  end
end

puts Views::Page.show
