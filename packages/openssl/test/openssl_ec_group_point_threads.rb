require "openssl"

# RFC 6979 A.2.5 public point; both workers share only Ruby value snapshots.
public_der = [
  "3059301306072a8648ce3d020106082a8648ce3d03010703420004" \
  "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6" \
  "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299"
].pack("H*")
point = OpenSSL::PKey.read(public_der).public_key.freeze
expected = point.to_octet_string(:compressed)
workers = 2.times.map do
  Thread.new do
    answers = []
    12.times do
      order = OpenSSL::PKey::EC::Group.new("prime256v1").order.to_i.to_s(16)
      bytes = point.to_octet_string(:compressed)
      point.to_octet_string(:uncompressed)
      answers << (bytes == expected && order == "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551")
    end
    answers.all?
  end
end
workers.each { |worker| p worker.value }
