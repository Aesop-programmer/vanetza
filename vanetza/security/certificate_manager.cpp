#include <vanetza/security/ecc_point.hpp>
#include <vanetza/security/basic_elements.hpp>
#include <vanetza/security/certificate_manager.hpp>
#include <vanetza/security/secured_message.hpp>
#include <vanetza/security/signature.hpp>
#include <vanetza/security/payload.hpp>
#include <cryptopp/osrng.h>
#include <cryptopp/oids.h>
#include <cryptopp/filters.h>
#include <cryptopp/hex.h>
#include <boost/format.hpp>

namespace vanetza
{
namespace security
{

CertificateManager::CertificateManager(const geonet::Timestamp& time_now) : m_time_now(time_now)
{
    // generate private key
    CryptoPP::OID oid(CryptoPP::ASN1::secp256r1());
    CryptoPP::AutoSeededRandomPool prng;
    m_private_key.Initialize(prng, oid);
    assert(m_private_key.Validate(prng, 3));

    // generate public key
    PublicKey public_key;
    m_private_key.MakePublicKey(public_key);
    assert(public_key.Validate(prng, 3));

    m_certificate = generate_certificate(public_key);
}

EncapConfirm CertificateManager::sign_message(const EncapRequest& request)
{
    EncapConfirm encap_confirm;
    // set secured message data
    encap_confirm.sec_packet.payload.type = PayloadType::Signed;
    encap_confirm.sec_packet.payload.buffer = std::move(request.plaintext_payload);
    // set header field data
    encap_confirm.sec_packet.header_fields.push_back(get_time()); // generation_time
    encap_confirm.sec_packet.header_fields.push_back((uint16_t) 36); // its_aid, according to TS 102 965, and ITS-AID_AssignedNumbers
    // TODO: header_fieds.signerInfo.type = certificate, fill in certificate or add certificate_digest_with_sha256
    SignerInfo signer_info = m_certificate;
    encap_confirm.sec_packet.header_fields.push_back(signer_info);

    // create trailer field to get the size in bytes
    size_t trailer_field_size = 0;
    {
        security::EcdsaSignature temp_signature;
        temp_signature.s.resize(field_size(PublicKeyAlgorithm::Ecdsa_Nistp256_With_Sha256));
        X_Coordinate_Only x_coordinate_only;
        x_coordinate_only.x.resize(field_size(PublicKeyAlgorithm::Ecdsa_Nistp256_With_Sha256));
        temp_signature.R = x_coordinate_only;

        security::TrailerField temp_trailer_field = temp_signature;

        trailer_field_size = get_size(temp_trailer_field);
    }

    // Covered by signature:
    //      SecuredMessage: protocol_version, header_fields (incl. its length), payload_field, trailer_field.trailer_field_type
    //      CommonHeader: complete
    //      ExtendedHeader: complete
    // p. 27 in TS 103 097 v1.2.1
    ByteBuffer data_buffer = request.plaintext_pdu;
    ByteBuffer data_payload = convert_for_signing(encap_confirm.sec_packet, TrailerFieldType::Signature, trailer_field_size);
    data_buffer.insert(data_buffer.end(), data_payload.begin(), data_payload.end());

    TrailerField trailer_field = sign_data(m_private_key, data_buffer);
    assert(get_size(trailer_field) == trailer_field_size);

    encap_confirm.sec_packet.trailer_fields.push_back(trailer_field);

    return std::move(encap_confirm);
}

DecapConfirm CertificateManager::verify_message(const DecapRequest& request)
{
    boost::optional<Certificate> certificate;
    for (auto& field : request.sec_packet.header_fields) {
        if (get_type(field) == HeaderFieldType::Signer_Info) {
            assert(SignerInfoType::Certificate == get_type(boost::get<SignerInfo>(field)));
            certificate = boost::get<Certificate>(boost::get<SignerInfo>(field));
            break;
        }
    }

    assert(certificate.is_initialized());
    // TODO (simon,markus): check certificate

    PublicKey public_key = get_public_key_from_certificate(certificate.get());

    // convert signature byte buffer to string
    SecuredMessage secured_message = std::move(request.sec_packet);
    std::list<TrailerField> trailer_fields;

    trailer_fields = secured_message.trailer_fields;
    std::string signature;

    assert(!trailer_fields.empty());

    std::list<TrailerField>::iterator it = trailer_fields.begin();
    signature = buffer_cast_to_string(extract_signature_buffer(*it));

    // convert message byte buffer to string
    std::string payload = buffer_cast_to_string(convert_for_signing(secured_message, get_type(*it), get_size(*it)));
    std::string pdu = buffer_cast_to_string(request.sec_pdu);

    std::string message = pdu + payload;

    // verify message signature
    bool result = false;
    CryptoPP::StringSource( signature+message, true,
                            new CryptoPP::SignatureVerificationFilter(Verifier(public_key), new CryptoPP::ArraySink((byte*)&result, sizeof(result)))
    );

    DecapConfirm decap_confirm;
    decap_confirm.plaintext_payload = std::move(request.sec_packet.payload.buffer);
    if (result) {
        decap_confirm.report = ReportType::Success;
    } else {
        decap_confirm.report = ReportType::False_Signature;
    }

    return std::move(decap_confirm);
}

const std::string CertificateManager::buffer_cast_to_string(const ByteBuffer& buffer)
{
    std::stringstream oss;
    std::copy(buffer.begin(), buffer.end(), std::ostream_iterator<ByteBuffer::value_type>(oss));
    return std::move(oss.str());
}

Certificate CertificateManager::generate_certificate(const PublicKey& public_key)
{
    Certificate certificate;

    // section 6.1 in TS 103 097 v1.2.1
    // TODO exchange with SignerInfoType::Certificate_Digest_With_ECDSA_P256 when there is a root certificate
    certificate.signer_info = std::nullptr_t(); // nullptr means self signed

    // section 6.3 in TS 103 097 v1.2.1
    certificate.subject_info.subject_type = SubjectType::Authorization_Ticket;
    // section 7.4.2 in TS 103 097 v1.2.1, subject_name implicit empty

    // section 7.4.1 in TS 103 097 v1.2.1
    // set subject attributes
    // set the verification_key
    Uncompressed coordinates;
    {
        coordinates.x.resize(32);
        coordinates.y.resize(32);
        public_key.GetPublicElement().x.Encode(&coordinates.x[0], 32);
        public_key.GetPublicElement().y.Encode(&coordinates.y[0], 32);

        assert(CryptoPP::SHA256::DIGESTSIZE == coordinates.x.size());
        assert(CryptoPP::SHA256::DIGESTSIZE == coordinates.y.size());
    }
    EccPoint ecc_point = coordinates;
    ecdsa_nistp256_with_sha256 ecdsa;
    ecdsa.public_key = ecc_point;
    VerificationKey verification_key;
    verification_key.key = ecdsa;
    certificate.subject_attributes.push_back(verification_key);

    // set assurance level
    certificate.subject_attributes.push_back(SubjectAssurance()); // TODO change confidence if necessary

    // section 6.7 in TS 103 097 v1.2.1
    // set validity restriction
    StartAndEndValidity start_and_end;
    start_and_end.start_validity = get_time_in_seconds();
    start_and_end.end_validity = get_time_in_seconds() + 3600 * 24; // add 1 day
    certificate.validity_restriction.push_back(start_and_end);

    // set signature
    ByteBuffer data_buffer = convert_for_signing(certificate);

    // Covered by signature:
    //      version, signer_field, subject_info,
    //      subject_attributes + length,
    //      validity_restriction + length
    // section 7.4 in TS 103 097 v1.2.1
    certificate.signature = std::move(sign_data(m_private_key, data_buffer));

    return std::move(certificate);
}

CertificateManager::PublicKey CertificateManager::get_public_key_from_certificate(const Certificate& certificate)
{
    // generate public key from certificate data (x_coordinate + y_coordinate)
    boost::optional<Uncompressed> public_key_coordinates;
    for (auto& attribute : certificate.subject_attributes) {
        if (get_type(attribute) == SubjectAttributeType::Verification_Key) {
            public_key_coordinates = boost::get<Uncompressed>(boost::get<ecdsa_nistp256_with_sha256>(boost::get<VerificationKey>(attribute).key).public_key);
            break;
        }
    }

    assert(public_key_coordinates.is_initialized());

    std::stringstream ss;
    for (uint8_t b : public_key_coordinates.get().x) {
        ss << boost::format("%02x") % (int)b;
    }
    std::string x_coordinate = ss.str();
    ss.str("");
    for (uint8_t b : public_key_coordinates.get().y) {
        ss << boost::format("%02x") % (int)b;
    }
    std::string y_coordinate = ss.str();

    CryptoPP::HexDecoder x_decoder, y_decoder;
    x_decoder.Put((byte*)x_coordinate.c_str(), x_coordinate.length());
    x_decoder.MessageEnd();
    y_decoder.Put((byte*)y_coordinate.c_str(), y_coordinate.length());
    y_decoder.MessageEnd();

    size_t len = x_decoder.MaxRetrievable();
    assert(len == CryptoPP::SHA256::DIGESTSIZE);
    len = y_decoder.MaxRetrievable();
    assert(len == CryptoPP::SHA256::DIGESTSIZE);

    CryptoPP::ECP::Point q;
    q.identity = false;
    q.x.Decode(x_decoder, len);
    q.y.Decode(y_decoder, len);

    PublicKey public_key;
    public_key.Initialize(CryptoPP::ASN1::secp256r1(), q);
    CryptoPP::AutoSeededRandomPool prng;
    assert(public_key.Validate(prng, 3));

    return public_key;
}

EcdsaSignature CertificateManager::sign_data(const PrivateKey& private_key, ByteBuffer data_buffer)
{
    CryptoPP::AutoSeededRandomPool prng;
    std::string signature;

    std::string data = buffer_cast_to_string(data_buffer);

    // calculate signature
    // TODO (simon, markus): write Source and Sink classes for ByteBuffer
    CryptoPP::StringSink* string_sink = new CryptoPP::StringSink(signature);
    Signer signer(private_key);
    CryptoPP::SignerFilter* signer_filter = new CryptoPP::SignerFilter(prng, std::move(signer), string_sink);

    CryptoPP::StringSource( data, true, signer_filter); // StringSource

    std::string signature_x = signature.substr(0, 32);
    std::string signature_s = signature.substr(32);

    EcdsaSignature ecdsa_signature;
    // set R
    X_Coordinate_Only coordinate;
    coordinate.x = ByteBuffer(signature_x.begin(), signature_x.end());
    ecdsa_signature.R = std::move(coordinate);
    // set s
    ByteBuffer trailer_field_buffer(signature_s.begin(), signature_s.end());
    ecdsa_signature.s = std::move(trailer_field_buffer);

    return ecdsa_signature;
}

Time64 CertificateManager::get_time()
{
    return ((Time64) m_time_now.raw()) * 1000 * 1000;
}

Time32 CertificateManager::get_time_in_seconds()
{
    return m_time_now.raw();
}

} // namespace security
} // namespace vanetza
