#include "PersistPch.h"
#include "Persist/ArchiveJson.h"

#include "Foundation/Log.h"
#include "Foundation/SmartPtr.h"

#include "Reflect/Object.h"
#include "Reflect/Registry.h"
#include "Reflect/Structure.h"

#if REFLECT_REFACTOR

#include <strstream>
#include <expat.h>

using namespace Helium;
using namespace Helium::Reflect;
using namespace Helium::Persist;

const uint32_t ArchiveJson::CURRENT_VERSION = 4;

#define PERSIST_ARCHIVE_VERBOSE

#pragma TODO("Structures still have an extra <Object Type=\"StructureType\">, eliminate that with removing the type check for Data")

class StlVectorPusher : NonCopyable
{
public:
    std::vector< ObjectPtr >& m_ObjectVector;

    explicit StlVectorPusher( std::vector< ObjectPtr >& objectVector )
        : m_ObjectVector( objectVector )
    {
    }

    void operator()( const ObjectPtr& object )
    {
        m_ObjectVector.push_back( object );
    }
};

class DynamicArrayPusher : NonCopyable
{
public:
    DynamicArray< ObjectPtr >& m_ObjectArray;

    explicit DynamicArrayPusher( DynamicArray< ObjectPtr >& objectArray )
        : m_ObjectArray( objectArray )
    {
    }

    void operator()( const ObjectPtr& object )
    {
        m_ObjectArray.Push( object );
    }
};

ArchiveJson::ArchiveJson( const FilePath& path, ByteOrder byteOrder )
: Archive( path, byteOrder )
, m_Version( CURRENT_VERSION )
, m_Size( 0 )
, m_Skip( false )
, m_Body( NULL )
{

}

ArchiveJson::ArchiveJson()
: Archive()
, m_Version( CURRENT_VERSION )
, m_Size( 0 )
, m_Skip( false )
, m_Body( NULL )
{

}

ArchiveJson::ArchiveJson( TCharStream *stream, bool write /*= false */ )
: Archive()
, m_Version( CURRENT_VERSION )
, m_Size( 0 )
, m_Skip( false )
, m_Body( NULL )
{
    HELIUM_ASSERT(stream);
    OpenStream(stream, write);
}

ArchiveJson::~ArchiveJson()
{
    if (m_Stream)
    {
        Close();
    }
}

void ArchiveJson::Open( bool write )
{
#ifdef PERSIST_ARCHIVE_VERBOSE
    Log::Debug(TXT("Opening file '%s'\n"), m_Path.c_str());
#endif

    Reflect::TCharStreamPtr stream = new TCharFileStream( m_Path, write );
    OpenStream( stream, write );
}

void ArchiveJson::OpenStream( TCharStream* stream, bool write )
{
    // save the mode here, so that we safely refer to it later.
    m_Mode = (write) ? ArchiveModes::Write : ArchiveModes::Read; 

    // open the stream, this is "our interface" 
    stream->Open(); 

    // Set precision
    stream->SetPrecision(32);

    // Setup stream
    m_Stream = stream;
}

void ArchiveJson::Close()
{
    HELIUM_ASSERT( m_Stream );

    m_Stream->Close(); 
    m_Stream = NULL; 
}

void ArchiveJson::Read()
{
    PERSIST_SCOPE_TIMER(( "Reflect - Json Read" ));

    ArchiveStatus info( *this, ArchiveStates::Starting );
    e_Status.Raise( info );

    m_Abort = false;

    ParseStream();
    ReadFileHeader(false);

    // deserialize main file objects
    {
        PERSIST_SCOPE_TIMER( ("Read Objects") );
        DeserializeArray(m_Objects, ArchiveFlags::Status);
    }

    info.m_State = ArchiveStates::ObjectProcessed;
    info.m_Progress = 100;
    e_Status.Raise( info );

    info.m_State = ArchiveStates::Complete;
    e_Status.Raise( info );
}

void ArchiveJson::Write()
{
    PERSIST_SCOPE_TIMER(( "Reflect - Json Write" ));

    ArchiveStatus info( *this, ArchiveStates::Starting );
    e_Status.Raise( info );


    WriteFileHeader();

    // serialize main file objects
    {
        PERSIST_SCOPE_TIMER( ("Write Objects") );
        SerializeArray(m_Objects, ArchiveFlags::Status);
    }

    WriteFileFooter();

    info.m_State = ArchiveStates::Complete;
    e_Status.Raise( info );
}

void ArchiveJson::SerializeInstance( Object* object )
{
    SerializeInstance( object, NULL );
}

void ArchiveJson::SerializeInstance( void* structure, const Structure* type )
{
    SerializeInstance( structure, type, NULL );
}

void ArchiveJson::SerializeInstance(Object* object, const tchar_t* fieldName)
{
    if ( object )
    {
        object->PreSerialize( NULL );
    }

    m_Indent.Push();
    m_Indent.Get( *m_Stream );
    *m_Stream << TXT( "<Object Type=\"" );
    if ( object )
    {
        const Class *c = object->GetClass();
        HELIUM_ASSERT(c);
        *m_Stream << c->m_Name;
    }
    
    *m_Stream << TXT( "\"" );

    if ( fieldName )
    {
        // our link back to the field we are nested in
        *m_Stream << TXT( " Name=\"" ) << fieldName << TXT( "\"" );
    }

    if ( !object || object->IsCompact() )
    {
        *m_Stream << TXT( ">" );
    }
    else
    {
        *m_Stream << TXT( ">\n" );
    }

    if ( object )
    {
        Data* data = SafeCast<Data>(object);

        if ( data )
        {
            data->Serialize(*this);
        }
        else
        {
            SerializeFields(object);
        }
    }

    if ( object && !object->IsCompact() )
    {
        m_Indent.Get(*m_Stream);
    }

    *m_Stream << TXT( "</Object>\n" );

    m_Indent.Pop();

    if ( object )
    {
        object->PostSerialize( NULL );
    }
}

void ArchiveJson::SerializeInstance( void* structure, const Structure* type, const tchar_t* fieldName )
{
    m_Indent.Push();
    m_Indent.Get( *m_Stream );
    *m_Stream << TXT( "<Object Type=\"" );
    *m_Stream << type->m_Name;
    *m_Stream << TXT( "\"" );

    if ( fieldName )
    {
        // our link back to the field we are nested in
        *m_Stream << TXT( " Name=\"" ) << fieldName << TXT( "\"" );
    }

    *m_Stream << TXT( ">\n" );

    SerializeFields(structure, type);

    m_Indent.Get(*m_Stream);
    *m_Stream << TXT( "</Object>\n" );

    m_Indent.Pop();
}

void ArchiveJson::SerializeFields( Object* object )
{
    const Class* type = object->GetClass();
    HELIUM_ASSERT(type != NULL);

    DynamicArray< Field >::ConstIterator itr = type->m_Fields.Begin();
    DynamicArray< Field >::ConstIterator end = type->m_Fields.End();
    for ( ; itr != end; ++itr )
    {
        const Field* field = &*itr;
        DataPtr data = object->ShouldSerialize( field );
        if ( data )
        {
            object->PreSerialize( field );
            SerializeInstance( data, field->m_Name );
            object->PostSerialize( field );

            // might be useful to cache the data object here
            data->Disconnect();
        }
    }
}

void ArchiveJson::SerializeFields( void* structure, const Structure* type )
{
    DynamicArray< Field >::ConstIterator itr = type->m_Fields.Begin();
    DynamicArray< Field >::ConstIterator end = type->m_Fields.End();
    for ( ; itr != end; ++itr )
    {
        const Field* field = &*itr;
        DataPtr data = field->ShouldSerialize( structure );
        if ( data )
        {
            SerializeInstance( data, field->m_Name );

            // might be useful to cache the data object here
            data->Disconnect();
        }
    }
}

void ArchiveJson::SerializeArray(const std::vector< ObjectPtr >& objects, uint32_t flags)
{
    SerializeArray( objects.begin(), objects.end(), flags );
}

void ArchiveJson::SerializeArray( const DynamicArray< ObjectPtr >& objects, uint32_t flags )
{
    SerializeArray( objects.Begin(), objects.End(), flags );
}

template< typename ConstIteratorType >
void ArchiveJson::SerializeArray( ConstIteratorType begin, ConstIteratorType end, uint32_t flags )
{
    size_t size = static_cast< size_t >( end - begin );

    ConstIteratorType itr = begin;
    for (int index = 0; itr != end; ++itr, ++index )
    {
        SerializeInstance(*itr, NULL);

        if ( flags & ArchiveFlags::Status )
        {
            ArchiveStatus info( *this, ArchiveStates::ObjectProcessed );
            info.m_Progress = (int)(((float)(index) / (float)size) * 100.0f);
            e_Status.Raise( info );
        }
    }

    if ( flags & ArchiveFlags::Status )
    {
        ArchiveStatus info( *this, ArchiveStates::ObjectProcessed );
        info.m_Progress = 100;
        e_Status.Raise( info );
    }
}

void ArchiveJson::DeserializeInstance(ObjectPtr& object)
{
    //
    // If we don't have an object allocated for deserialization, pull one from the stream
    //

    if (!object.ReferencesObject())
    {
        object = Allocate();
    }

    //
    // We should now have an instance (unless data was skipped)
    //

    if (object.ReferencesObject())
    {
#ifdef PERSIST_ARCHIVE_VERBOSE
        m_Indent.Get(stdout);
        Log::Print(TXT("Deserializing %s\n"), object->GetClass()->m_Name);
        m_Indent.Push();
#endif

        object->PreDeserialize( NULL );

        Data* data = SafeCast<Data>(object);

        if ( data )
        {
#pragma TODO("Make sure this string copy goes away when replace the stl stream APIs")
            tstring body ( m_Iterator.GetCurrent()->m_Body.GetData(), m_Iterator.GetCurrent()->m_Body.GetSize() );
            tstringstream stringStream ( body );
            TCharStream stream ( &stringStream, false );

            const DeserializingField *deserializing_field = GetDeserializingField();
            if (deserializing_field)
            {
                //data->ConnectField(deserializing_field->m_Instance, deserializing_field->m_Field);
            }

            Helium::JsonElement *element = m_Iterator.GetCurrent();

            m_Body = &stream;
            data->Deserialize(*this);
            m_Body = NULL;

            // I don't like doing this change, but structures/objects call DeserializeInstance
            // but simple data doesn't.
            if (m_Iterator.GetCurrent() == element)
            {
                m_Iterator.Advance();
            }
        }
        else
        {
            DeserializeFields(object);
        }

        object->PostDeserialize( NULL );

#ifdef PERSIST_ARCHIVE_VERBOSE
        m_Indent.Pop();
#endif
    }
}

void ArchiveJson::DeserializeInstance( void* structure, const Structure* type )
{
#ifdef PERSIST_ARCHIVE_VERBOSE
    m_Indent.Get(stdout);
    Log::Print(TXT("Deserializing %s\n"), type->m_Name);
    m_Indent.Push();
#endif

    // pmd - Step into the structure field
    m_Iterator.Advance();
    DeserializeFields(structure, type);

#ifdef PERSIST_ARCHIVE_VERBOSE
    m_Indent.Pop();
#endif
}

void ArchiveJson::DeserializeFields(Object* object)
{
    if ( m_Iterator.GetCurrent()->GetFirstChild() )
    {
        // advance to the first child
        m_Iterator.Advance();
        
        size_t deserializing_field_index = m_DeserializingFieldStack.GetSize();
        //DeserializingField *deserializing_field = m_DeserializingFieldStack.New();
        m_DeserializingFieldStack.New();
        //HELIUM_ASSERT(deserializing_field);
        HELIUM_ASSERT(m_DeserializingFieldStack.GetSize() == (deserializing_field_index + 1));
        //deserializing_field->m_Instance = structure;
        m_DeserializingFieldStack[deserializing_field_index].m_Instance = object;
        for ( JsonElement* sibling = m_Iterator.GetCurrent(); sibling != NULL; sibling = sibling->GetNextSibling() )
        {
            HELIUM_ASSERT( m_Iterator.GetCurrent() == sibling );

            const String* fieldNameStr = sibling->GetAttributeValue( Name( TXT("Name") ) );
            uint32_t fieldNameCrc = fieldNameStr ? Crc32( fieldNameStr->GetData() ) : 0x0;

            const Class* type = object->GetClass();
            HELIUM_ASSERT( type );

            ObjectPtr unknown;
            const Field* field = type->FindFieldByName(fieldNameCrc);
            //deserializing_field->m_Field = field;
            m_DeserializingFieldStack[deserializing_field_index].m_Field = field;
            if ( field )
            {
#ifdef PERSIST_ARCHIVE_VERBOSE
                m_Indent.Get(stdout);
                Log::Print(TXT("Deserializing field %s\n"), field->m_Name);
                m_Indent.Push();
#endif

                // pull and object and downcast to data
                DataPtr latentData = SafeCast<Data>( Allocate() );
                if (!latentData.ReferencesObject())
                {
                    // this should never happen, the type id read from the file is bogus
                    throw Persist::TypeInformationException( TXT( "Unknown data for field %s (%s)" ), field->m_Name, m_Path.c_str() );
#pragma TODO("Support blind data")
                }

                // if the types match we are a natural fit to just deserialize directly into the field data
                if ( field->m_DataClass == field->m_DataClass )
                {
                    // set data pointer
                    latentData->ConnectField( object, field );

                    // process natively
                    object->PreDeserialize( field );
                    DeserializeInstance( (ObjectPtr&)latentData );
                    object->PostDeserialize( field );

                    // disconnect
                    latentData->Disconnect();
                }
                else
				{
#pragma TODO("Support blind data")
				}
            }
            else // else the field does not exist in the current class anymore
            {
                try
                {
                    DeserializeInstance( unknown );
                }
                catch (Reflect::LogisticException& ex)
                {
                    Log::Debug( TXT( "Unable to deserialize %s::%s, discarding: %s\n" ), type->m_Name, field->m_Name, ex.What());
                }
            }

            if ( unknown.ReferencesObject() )
            {
                // attempt processing
                object->ProcessUnknown( unknown, field ? Crc32( field->m_Name ) : 0 );
            }

#ifdef PERSIST_ARCHIVE_VERBOSE
            m_Indent.Pop();
#endif
        }
        m_DeserializingFieldStack.Pop();
    }
    else
    {
        // advance to the next element
        m_Iterator.Advance();
    }
}

void ArchiveJson::DeserializeFields( void* structure, const Structure* type )
{
    if ( m_Iterator.GetCurrent()->GetFirstChild() )
    {
        // advance to the first child
        m_Iterator.Advance();

        size_t deserializing_field_index = m_DeserializingFieldStack.GetSize();
        //DeserializingField *deserializing_field = m_DeserializingFieldStack.New();
        m_DeserializingFieldStack.New();
        //HELIUM_ASSERT(deserializing_field);
        HELIUM_ASSERT(m_DeserializingFieldStack.GetSize() == (deserializing_field_index + 1));
        //deserializing_field->m_Instance = structure;
        m_DeserializingFieldStack[deserializing_field_index].m_Instance = structure;
        for ( JsonElement* sibling = m_Iterator.GetCurrent(); sibling != NULL; sibling = sibling->GetNextSibling() )
        {
            HELIUM_ASSERT( m_Iterator.GetCurrent() == sibling );

            const String* fieldNameStr = sibling->GetAttributeValue( Name( TXT("Name") ) );
            uint32_t fieldNameCrc = fieldNameStr ? Crc32( fieldNameStr->GetData() ) : 0x0;

            const Field* field = type->FindFieldByName(fieldNameCrc);
            m_DeserializingFieldStack[deserializing_field_index].m_Field = field;
            if ( field )
            {
#ifdef PERSIST_ARCHIVE_VERBOSE
                m_Indent.Get(stdout);
                Log::Debug(TXT("Deserializing field %s\n"), field->m_Name);
                m_Indent.Push();
#endif

                // pull and structure and downcast to data
                DataPtr latentData = SafeCast<Data>( Allocate() );
                if (!latentData.ReferencesObject())
                {
                    // this should never happen, the type id read from the file is bogus
                    throw Persist::TypeInformationException( TXT( "Unknown data for field %s (%s)" ), field->m_Name, m_Path.c_str() );
#pragma TODO("Support blind data")
                }

                // if the types match we are a natural fit to just deserialize directly into the field data
                if ( field->m_DataClass == field->m_DataClass )
                {
                    // set data pointer
                    latentData->ConnectField( structure, field );

                    // process natively
                    DeserializeInstance( (ObjectPtr&)latentData );

                    // disconnect
                    latentData->Disconnect();
                }
                else
				{
#pragma TODO("Support blind data")
                }
            }

#ifdef PERSIST_ARCHIVE_VERBOSE
            m_Indent.Pop();
#endif
        }
        // else skip this entire node
        //
        //
        m_DeserializingFieldStack.Pop();
    }
    else
    {
        // advance to the next element
        m_Iterator.Advance();
    }
}

void ArchiveJson::DeserializeArray( std::vector< ObjectPtr >& objects, uint32_t flags )
{
    DeserializeArray( StlVectorPusher( objects ), flags );
}

void ArchiveJson::DeserializeArray( DynamicArray< ObjectPtr >& objects, uint32_t flags )
{
    DeserializeArray( DynamicArrayPusher( objects ), flags );
}

template< typename ArrayPusher >
void ArchiveJson::DeserializeArray( ArrayPusher& push, uint32_t flags )
{
    if ( m_Iterator.GetCurrent()->GetFirstChild() )
    {
        // advance to the first child (the first array element)
        m_Iterator.Advance();

#ifdef PERSIST_ARCHIVE_VERBOSE
        m_Indent.Get(stdout);
        Log::Print(TXT("Deserializing objects\n"));
        m_Indent.Push();
#endif

        for ( JsonElement* sibling = m_Iterator.GetCurrent(); sibling != NULL; sibling = sibling->GetNextSibling() )
        {
            HELIUM_ASSERT( m_Iterator.GetCurrent() == sibling );

            ObjectPtr object = Allocate();
            if (object.ReferencesObject())
            {
                // A bit of a hack to handle structs.
                StructureData* structure_data = SafeCast<StructureData>(object);
                if ( structure_data )
                {
                    //deserializing_field->m_Instance = structure_data->m_Data.Get(deserializing_field->m_Field->m_Size);
                    const DeserializingField *deserializing_field = GetDeserializingField();
                    HELIUM_ASSERT(deserializing_field);
                    structure_data->AllocateForArrayEntry(deserializing_field->m_Instance, deserializing_field->m_Field);
                }
                else
                {
                    //deserializing_field->m_Instance = object.Get();
                }

                DeserializeInstance(object);

                if ( object->IsClass( m_SearchClass ) )
                {
                    m_Skip = true;
                }

                if ( flags & ArchiveFlags::Status )
                {
                    ArchiveStatus info( *this, ArchiveStates::ObjectProcessed );
#pragma TODO("Update progress value for inter-array processing")
                    //info.m_Progress = (int)(((float)(current - start_offset) / (float)m_Size) * 100.0f);
                    e_Status.Raise( info );

                    m_Abort |= info.m_Abort;
                }
            }

            push( object );
        }
    }
    else
    {
        // advance to the next element
        m_Iterator.Advance();
    }

#ifdef PERSIST_ARCHIVE_VERBOSE
    m_Indent.Pop();
#endif

    if ( flags & ArchiveFlags::Status )
    {
        ArchiveStatus info( *this, ArchiveStates::ObjectProcessed );
        info.m_Progress = 100;
        e_Status.Raise( info );
    }
}

ObjectPtr ArchiveJson::Allocate()
{
    ObjectPtr object;

    // find type
    const String* typeStr = m_Iterator.GetCurrent()->GetAttributeValue( Name( TXT("Type") ) );
    uint32_t typeCrc = typeStr ? Crc32( typeStr->GetData() ) : 0x0;

    // A null type name CRC indicates that a null reference was serialized, so no type lookup needs to be performed.
    const Class* type = NULL;
    if ( typeCrc != 0 )
    {
        type = Reflect::Registry::GetInstance()->GetClass( typeCrc );
    }

    if (type)
    {
        // allocate instance by name
        object = Registry::GetInstance()->CreateInstance( type );
    }

    // if we failed
    if (!object.ReferencesObject())
    {
        // if you see this, then data is being lost because:
        //  1 - a type was completely removed from the codebase
        //  2 - a type was not found because its type library is not registered
        Log::Debug( TXT( "Unable to create object of type %s, skipping...\n" ), type ? type->m_Name : TXT("Unknown") );

        // skip past this object, skipping our children
        m_Iterator.Advance( true );
#pragma TODO("Support blind data")
    }

    return object;
}

void ArchiveJson::ToString( Object* object, tstring& xml )
{
    std::vector< ObjectPtr > objects(1);
    objects[0] = object;
    return ToString( objects, xml );
}

ObjectPtr ArchiveJson::FromString( const tstring& xml, const Class* searchClass )
{
    if ( searchClass == NULL )
    {
        searchClass = Reflect::GetClass<Object>();
    }
    
    tstringstream strStream;
    strStream << xml;

    ArchiveJson archive(new Reflect::TCharStream(&strStream, false), false);
    archive.m_SearchClass = searchClass;
    archive.Read();
    archive.Close();

    std::vector< ObjectPtr >::iterator itr = archive.m_Objects.begin();
    std::vector< ObjectPtr >::iterator end = archive.m_Objects.end();
    for ( ; itr != end; ++itr )
    {
        if ((*itr)->IsClass(searchClass))
        {
            return *itr;
        }
    }

    return NULL;
}

void ArchiveJson::ToString( const std::vector< ObjectPtr >& objects, tstring& xml )
{
    //ArchiveJson archive;
    tstringstream strStream;

    ArchiveJson archive(new Reflect::TCharStream(&strStream, false), true);
    archive.m_Objects = objects;
    archive.Write();
    archive.Close();
    xml = strStream.str();
}

void ArchiveJson::FromString( const tstring& xml, std::vector< ObjectPtr >& objects )
{
    //ArchiveJson archive;
    tstringstream strStream;
    strStream << xml;

    ArchiveJson archive(new Reflect::TCharStream(&strStream, false), false);
    archive.Read();
    archive.Close();

    objects = archive.m_Objects;
}

void Helium::Reflect::ArchiveJson::WriteFileHeader()
{
    *m_Stream << TXT( "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" );
    *m_Stream << TXT( "<Reflect FileFormatVersion=\"" ) << m_Version << TXT( "\">\n" );
}

void Helium::Reflect::ArchiveJson::WriteFileFooter()
{
    *m_Stream << TXT( "</Reflect>\n\0" );
}

void Helium::Reflect::ArchiveJson::ReadFileHeader(bool _reparse)
{
    if (_reparse)
    {
        ParseStream();
    }

    // read file format version attribute
    const String* version = m_Iterator.GetCurrent()->GetAttributeValue( Name( TXT( "FileFormatVersion" ) ) );
    if ( version )
    {
        tstringstream str ( version->GetData() );
        str >> m_Version;
    }
}

void Helium::Reflect::ArchiveJson::ReadFileFooter()
{

}

void Helium::Reflect::ArchiveJson::ParseStream()
{
    // determine the size of the input stream
    m_Stream->SeekRead(0, std::ios_base::end);
    m_Size = m_Stream->TellRead();
    m_Stream->SeekRead(0, std::ios_base::beg);

    // fail on an empty input stream
    if ( m_Size == 0 )
    {
        throw Persist::StreamException( TXT( "Input stream is empty" ) );
    }

    // while there is data, parse buffer
    {
        PERSIST_SCOPE_TIMER( ("Parse Json") );

		bool parsedOk = true;
        long step = 0;
        const unsigned bufferSizeInBytes = 4096;
        char* buffer = static_cast< char* >( alloca( bufferSizeInBytes ) );
        while (parsedOk && !m_Stream->Fail() && !m_Abort)
        {
            m_Progress = (int)(((float)(step++ * bufferSizeInBytes) / (float)m_Size) * 100.0f);

            // divide by the character size so wide char builds don't override the allocation
            //  stream objects read characters, not byte-by-byte
            m_Stream->ReadBuffer(buffer, bufferSizeInBytes / sizeof(tchar_t));
            int bytesRead = static_cast<int>(m_Stream->ElementsRead());

            parsedOk = m_Document.Parse(buffer, bytesRead * sizeof(tchar_t), bytesRead == 0);
        }
    }

    m_Iterator.SetCurrent( m_Document.GetRoot() );
}

void Helium::Reflect::ArchiveJson::WriteSingleObject( Object& object )
{
    SerializeInstance(&object);
}

bool Helium::Reflect::ArchiveJson::BeginReadingSingleObjects()
{
    bool return_value = m_Iterator.GetCurrent()->GetFirstChild() != NULL;
    m_Iterator.Advance();
    return return_value;
}

bool Helium::Reflect::ArchiveJson::ReadSingleObject( ObjectPtr& object )
{
    bool return_value = m_Iterator.GetCurrent()->GetNextSibling() != NULL;
    DeserializeInstance(object);
    return return_value;
}

void Helium::Reflect::ArchiveJson::ReadString( tstring &str )
{
    std::streamsize size = GetStream().ElementsAvailable(); 
    str.resize( (size_t)size );
    GetStream().ReadBuffer( const_cast<tchar_t*>( (str).c_str() ), size );
}

void Helium::Reflect::ArchiveJson::WriteString( const tstring &str )
{
    GetStream() << TXT( "<![CDATA[" ) << str << TXT( "]]>" );
}

#endif