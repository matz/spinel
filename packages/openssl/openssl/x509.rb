# OpenSSL::X509::Certificate -- a certificate is its DER, the way a key is its
# bytes (#9341). The spellings are CRuby's: new and load read PEM or DER, and
# to_der and to_pem write it back out.
#
# A certificate is also built here, as CRuby builds one: Certificate.new with
# no argument, the setters (version, serial, subject, issuer, not_before,
# not_after, public_key), then #sign with an EC or RSA key; and its fields
# are read back with the same names, from the DER, so a certificate that was
# loaded and one that was built answer alike. Extensions are not here, to
# add or to read; neither is the rest of X509 this package does not define,
# which is CRuby's NoMethodError.
#
# The PEM is parsed once, in C, where it crosses the boundary, and nothing on
# the C side outlives the call: there is no X509 * to free and no handle to
# run out of. An abandoned certificate costs its bytes. A certificate that
# has not been signed yet cannot be encoded at all, so it is its fields until
# #sign makes its DER from them; a change to one that has a DER is made to the
# DER, which keeps its old signature until it is signed again, as in CRuby.
module OpenSSL
  module X509
    class CertificateError < OpenSSLError
    end

    # What X509::Store#add_cert raises (openssl.rb).
    class StoreError < OpenSSLError
    end

    class NameError < OpenSSLError
    end

    # A distinguished name, which is its DER, as a certificate is: each entry
    # added is added to the DER in C, and to_s, to_a and the comparisons read
    # it back.
    class Name
      DEFAULT_OBJECT_TYPE = ASN1::UTF8STRING
      # The type an entry is given when none is named, by its field: CRuby's
      # template, UTF8STRING for any field not in it.
      OBJECT_TYPE_TEMPLATE = Hash.new(DEFAULT_OBJECT_TYPE).update(
        "C" => ASN1::PRINTABLESTRING,
        "countryName" => ASN1::PRINTABLESTRING,
        "serialNumber" => ASN1::PRINTABLESTRING,
        "dnQualifier" => ASN1::PRINTABLESTRING,
        "DC" => ASN1::IA5STRING,
        "domainComponent" => ASN1::IA5STRING,
        "emailAddress" => ASN1::IA5STRING
      )

      # "/CN=x/O=y", or "CN=x, O=y", as CRuby's parse_openssl reads it.
      def self.parse_openssl(str, template = OBJECT_TYPE_TEMPLATE)
        str = OpenSSL.send(:comparison_string, str)
        ary = if str.start_with?("/")
                str.byteslice(1, str.bytesize - 1).split("/").map { |i| i.split("=", 2) }
              else
                str.split(",").map { |i| i.strip.split("=", 2) }
              end
        new(ary, template)
      end

      def self.parse(str, template = OBJECT_TYPE_TEMPLATE) = parse_openssl(str, template)

      # No argument, an Array of [field, value] or [field, value, type], or
      # a name's DER.
      def initialize(name = nil, template = OBJECT_TYPE_TEMPLATE)
        @der = "\x30\x00".b
        if name.is_a?(Array)
          name.each { |e| add_entry(e[0], e[1], e[2] || template[e[0]]) }
        elsif !name.nil?
          der = Native.x509_name_der(OpenSSL.send(:comparison_string, name))
          raise NameError, Native.last_error if der.empty?
          @der = der
        end
      end

      def add_entry(oid, value, type = nil)
        oid = OpenSSL.send(:comparison_string, oid)
        value = OpenSSL.send(:comparison_string, value)
        type = OBJECT_TYPE_TEMPLATE[oid] if type.nil?
        der = Native.x509_name_add(@der, oid, value, type)
        raise NameError, Native.last_error if der.empty?
        @der = der
        self
      end

      def to_s = Native.x509_name_oneline(@der)

      # [[short name, value, type], ...], as CRuby answers.
      def to_a
        blob = Native.x509_name_entries(@der)
        out = []
        at = 0
        while at < blob.bytesize
          parts = []
          3.times do
            len = blob.byteslice(at, 4).unpack1("N")
            parts << blob.byteslice(at + 4, len)
            at += 4 + len
          end
          out << [parts[0], parts[1], parts[2].to_i]
        end
        out
      end

      def to_der = @der.dup

      def cmp(other)
        raise TypeError, "wrong argument type #{other.class} (expected OpenSSL/X509/NAME)" unless other.is_a?(Name)
        Native.x509_name_cmp(@der, other.to_der)
      end

      def <=>(other)
        return nil unless other.is_a?(Name)
        cmp(other)
      end

      def ==(other)
        return false unless other.is_a?(Name)
        cmp(other) == 0
      end
    end

    class Certificate
      # Every certificate in a PEM string, in order: for a fullchain.pem, the
      # leaf and then the certificates that issued it. A DER string holds one
      # certificate, and answers an Array of that one, as in CRuby.
      def self.load(data)
        blob = Native.x509_load(data)
        raise CertificateError, Native.last_error if blob.empty?
        certs = []
        at = 0
        while at < blob.bytesize
          len = blob.byteslice(at, 4).unpack1("N")
          certs << new(blob.byteslice(at + 4, len))
          at += 4 + len
        end
        certs
      end

      # PEM or DER, as in CRuby, and the DER is what is kept; or nothing, for
      # a certificate to build, which is its fields until #sign.
      def initialize(data = nil)
        @fields = {}
        if data.nil?
          @der = nil
        else
          @der = Native.x509_der(data)
          raise CertificateError, Native.last_error if @der.empty?
        end
      end

      # A copy, as CRuby answers a new String each call: a caller that changes
      # what it was given does not change the certificate. A certificate that
      # has never been signed has no DER, and raises what CRuby raises.
      def to_der
        return @der.dup if @der
        der = Native.x509_build("", field_records, "", "", "")
        raise CertificateError, Native.last_error if der.empty?
        der
      end

      def to_pem
        Native.x509_pem(to_der)
      end

      def version = @der ? field("v").to_i : (@fields["v"] || "0").to_i

      def version=(version)
        n = version.to_int
        raise CertificateError, "version must be >= 0!" if n < 0
        change("v", n.to_s)
        version
      end

      def serial = BN.new(@der ? field("s") : (@fields["s"] || "0"))

      def serial=(serial)
        n = serial.is_a?(BN) ? serial.to_i : serial.to_int
        change("s", n.to_s)
        serial
      end

      def subject = name_field("S")
      def issuer = name_field("I")

      def subject=(name)
        change("S", name_der(name))
        name
      end

      def issuer=(name)
        change("I", name_der(name))
        name
      end

      def not_before = time_field("b")
      def not_after = time_field("a")

      def not_before=(time)
        change("b", seconds(time).to_s)
        time
      end

      def not_after=(time)
        change("a", seconds(time).to_s)
        time
      end

      # The key the certificate carries, public only, as CRuby answers it.
      def public_key
        spki = @der ? field("k") : (@fields["k"] || "")
        raise CertificateError, "unable to load the certificate's public key" if spki.empty?
        PKey.read(spki)
      end

      def public_key=(key)
        change("k", spki(key))
        key
      end

      # Signs the certificate with the key's private half over the digest,
      # given by its name or as an OpenSSL::Digest, and answers self.
      def sign(key, digest)
        curve, bytes = key_material(key)
        raise ArgumentError, "private key is needed" unless key.private?
        md = digest.is_a?(OpenSSL::Digest) ? digest.name : OpenSSL.send(:comparison_string, digest)
        der = Native.x509_build(@der || "", @der ? "" : field_records, curve, bytes, md)
        raise CertificateError, Native.last_error if der.empty?
        @der = der
        @fields = {}
        self
      end

      # Whether the key's public half verifies the certificate's signature.
      def verify(key)
        rc = Native.x509_verify(to_der, spki(key))
        raise CertificateError, Native.last_error if rc < 0
        rc == 1
      end

      private

      # A change to a certificate with a DER is made to the DER; one without
      # is held until #sign builds the DER.
      def change(code, bytes)
        if @der
          der = Native.x509_build(@der, record(code, bytes), "", "", "")
          raise CertificateError, Native.last_error if der.empty?
          @der = der
        else
          @fields[code] = bytes
        end
      end

      def record(code, bytes) = "#{code}#{[bytes.bytesize].pack("N")}".b + bytes.b

      def field_records
        out = "".b
        @fields.each { |code, bytes| out << record(code, bytes) }
        out
      end

      def field(code)
        out = Native.x509_field(@der, code.ord)
        raise CertificateError, Native.last_error if out.empty? && code != "b" && code != "a"
        out
      end

      def name_field(code)
        der = @der ? field(code) : @fields[code]
        der.nil? ? Name.new : Name.new(der)
      end

      def time_field(code)
        secs = @der ? field(code) : (@fields[code] || "")
        raise ASN1::ASN1Error, "ASN1_TIME_to_tm" if secs.empty?
        Time.at(secs.to_i).utc
      end

      def name_der(name)
        raise TypeError, "wrong argument type #{name.class} (expected OpenSSL/X509/NAME)" unless name.is_a?(Name)
        name.to_der
      end

      def seconds(time)
        time.is_a?(Integer) ? time : time.to_i
      end

      # The SubjectPublicKeyInfo DER of an EC or RSA key's public half.
      def spki(key)
        if key.is_a?(PKey::EC)
          key.public_to_der
        elsif key.is_a?(PKey::RSA)
          key.public_key.to_der
        else
          raise TypeError, "wrong argument type #{key.class} (expected OpenSSL/PKey)"
        end
      end

      # A private key as the C takes it, as SSLContext hands one over: an EC
      # key as its curve and scalar, an RSA key as no curve and its PKCS#1
      # DER.
      def key_material(key)
        if key.is_a?(PKey::EC)
          [key.curve, key.private_key_bytes]
        elsif key.is_a?(PKey::RSA)
          ["", key.to_der]
        else
          raise TypeError, "wrong argument type #{key.class} (expected OpenSSL/PKey)"
        end
      end
    end
  end
end
