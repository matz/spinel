# binmode: true reads the child's output as bytes, as CRuby's binary pipes
# do: capture3, capture2 and capture2e accepted it and read text, so the
# output was UTF-8 and a Regexp search over it counted characters.
require "open3"

out, err, st = Open3.capture3("printf", "\\377\\000\\303\\251", binmode: true)
p out, out.encoding, out.bytesize, err, err.encoding, st.success?
out, st = Open3.capture2("printf", "\\200\\001", binmode: true)
p out, out.encoding, st.success?
out, st = Open3.capture2e("sh", "-c", "printf '\\377'; printf '\\376' 1>&2",
                          binmode: true)
p out.bytes.sort, out.encoding
out, err, = Open3.capture3("sh", "-c", "printf '\\001'; printf '\\002' 1>&2",
                           binmode: true)
p out, err, out.encoding, err.encoding

# bytes in through stdin_data come back unchanged
fed = [0, 255, 195, 169, 10].pack("C*")
out, st = Open3.capture2("cat", stdin_data: fed, binmode: true)
p out == fed, out.encoding

# without binmode the output is text, as before
out, = Open3.capture3("printf", "caf\\303\\251")
p out, out.encoding, out.length
