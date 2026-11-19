# A String Range with two all-digit ends holds the numbers between, each
# zero-padded to the begin's width (String#upto): include? answers from the
# asked string alone, as the walk did.
rs = [["1", "200000"], ["9", "11"], ["1", "010"], ["001", "120"], ["0", "0"], ["5", "3"], ["10", "20"], ["000", "005"]]
xs = ["0", "1", "01", "001", "9", "10", "11", "010", "100", "120", "121", "199999", "200000", "200001", "005", "5", "0005", "", "1a", "99", "12"]
rs.each do |b, e|
  [false, true].each do |ex|
    r = ex ? (b...e) : (b..e)
    p [b, e, ex, xs.map { |x| r.include?(x) ? 1 : 0 }.join]
  end
end
