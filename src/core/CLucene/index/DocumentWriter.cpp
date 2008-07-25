/*------------------------------------------------------------------------------
* Copyright (C) 2003-2006 Ben van Klinken and the CLucene Team
* 
* Distributable under the terms of either the Apache License (Version 2.0) or 
* the GNU Lesser General Public License, as specified in the COPYING file.
------------------------------------------------------------------------------*/
#include "CLucene/_ApiHeader.h"

#include "CLucene/store/Directory.h"
#include "CLucene/store/IndexOutput.h"
#include "CLucene/util/Array.h"
#include "CLucene/util/Misc.h"
#include "CLucene/util/Reader.h"
#include "CLucene/document/Field.h"
#include "CLucene/document/Document.h"
#include "_FieldInfos.h"
#include "IndexWriter.h"
#include "_FieldsWriter.h"
#include "Term.h"
#include "_TermInfo.h"
#include "_TermVector.h"
#include "_TermInfosWriter.h"
#include "CLucene/analysis/AnalysisHeader.h"
#include "CLucene/search/Similarity.h"
#include "_TermInfosWriter.h"
#include "_FieldsWriter.h"
#include "_DocumentWriter.h"

CL_NS_USE(util)
CL_NS_USE(store)
CL_NS_USE(analysis)
CL_NS_USE(document)
CL_NS_DEF(index)

/*Posting*/

DocumentWriter::Posting::Posting(Term* t, const int32_t position, TermVectorOffsetInfo* offset)
{
//Func - Constructor
//Pre  - t contains a valid reference to a Term
//Post - Instance has been created
	freq = 1;
	
	term = _CL_POINTER(t);
	positions.values = (int32_t*)malloc(sizeof(int32_t));
	positions.values[0] = position;
	positions.length = 1;
	
	if ( offset != NULL ){
		this->offsets.values = (TermVectorOffsetInfo*)malloc(sizeof(TermVectorOffsetInfo));
		this->offsets.values[0] = *offset;
		this->offsets.length = 1;
	}
}
DocumentWriter::Posting::~Posting(){
//Func - Destructor
//Pre  - true
//Post - The instance has been destroyed

	free(this->positions.values);
	if ( this->offsets.values != NULL )
		free(this->offsets.values);
	_CLDECDELETE(this->term);
}



DocumentWriter::DocumentWriter(Directory* d, Analyzer* a, CL_NS(search)::Similarity* sim, const int32_t mfl):
		analyzer(a),
		directory(d),
		fieldInfos(NULL),
		maxFieldLength(mfl),
		similarity(sim),
		termIndexInterval( IndexWriter::DEFAULT_TERM_INDEX_INTERVAL ),
		postingTable(_CLNEW PostingTableType),
		fieldLengths(NULL),
		fieldPositions(NULL),
		fieldBoosts(NULL),
		termBuffer(_CLNEW Term){
//Pre  - d contains a valid reference to a Directory
//       d contains a valid reference to a Analyzer
//       mfl > 0 and contains the maximum field length
//Post - Instance has been created
 
CND_PRECONDITION(((mfl > 0) || (mfl == IndexWriter::FIELD_TRUNC_POLICY__WARN)),
 "mfl is 0 or smaller than IndexWriter::FIELD_TRUNC_POLICY__WARN")
 
   fieldInfos     = NULL;
   fieldLengths   = NULL;
}

DocumentWriter::DocumentWriter(CL_NS(store)::Directory* d, CL_NS(analysis)::Analyzer* a, IndexWriter* writer):
		analyzer(a),
		directory(d),
		fieldInfos(NULL),
		maxFieldLength(writer->getMaxFieldLength()),
		similarity(writer->getSimilarity()),
		termIndexInterval( writer->getTermIndexInterval() ),
		postingTable(_CLNEW PostingTableType),
		fieldLengths(NULL),
		fieldPositions(NULL),
		fieldBoosts(NULL),
		termBuffer(_CLNEW Term){
//Pre  - d contains a valid reference to a Directory
//       d contains a valid reference to a Analyzer
//       mfl > 0 and contains the maximum field length
//Post - Instance has been created
 
CND_PRECONDITION(((maxFieldLength > 0) || (maxFieldLength == IndexWriter::FIELD_TRUNC_POLICY__WARN)),
 "mfl is 0 or smaller than IndexWriter::FIELD_TRUNC_POLICY__WARN")
 
   fieldInfos     = NULL;
   fieldLengths   = NULL;

}

DocumentWriter::~DocumentWriter(){
//Func - Destructor
//Pre  - true
//Post - The instance has been destroyed
	clearPostingTable();
	_CLDELETE( fieldInfos );
	_CLDELETE_ARRAY(fieldLengths);
	_CLDELETE_ARRAY(fieldPositions);
	_CLDELETE_ARRAY(fieldBoosts);
	_CLDELETE_ARRAY(fieldOffsets);

	_CLDECDELETE(termBuffer);
	_CLDELETE(postingTable);
}

void DocumentWriter::clearPostingTable(){
	PostingTableType::iterator itr = postingTable->begin();
	while ( itr != postingTable->end() ){
		_CLDELETE(itr->second);
		_CLLDECDELETE(itr->first);

		++itr;
	}
	postingTable->clear();
}

void DocumentWriter::addDocument(const char* segment, Document* doc) {
    CND_PRECONDITION(fieldInfos==NULL, "fieldInfos!=NULL")

	// write field names
	fieldInfos = _CLNEW FieldInfos();
	fieldInfos->add(doc);
	
	const char* buf = Misc::segmentname(segment, ".fnm");
	fieldInfos->write(directory, buf);
	_CLDELETE_CaARRAY(buf);

	// write field values
	FieldsWriter fieldsWriter(directory, segment, fieldInfos);
	try {
		fieldsWriter.addDocument(doc);
	} _CLFINALLY( fieldsWriter.close() );
      
	// invert doc into postingTable
	clearPostingTable();			  // clear postingTable
	
	size_t size = fieldInfos->size();
	fieldLengths = _CL_NEWARRAY(int32_t,size);	// init fieldLengths
	fieldPositions = _CL_NEWARRAY(int32_t,size);  // init fieldPositions
	fieldOffsets = _CL_NEWARRAY(int32_t,size);    // init fieldOffsets
	memset(fieldPositions, 0, sizeof(int32_t) * size);
      
	//initialise fieldBoost array with default boost
	int32_t fbl = fieldInfos->size();
	float_t fbd = doc->getBoost();
	fieldBoosts = _CL_NEWARRAY(float_t,fbl);	  // init fieldBoosts
	{ //msvc6 scope fix
		for ( int32_t i=0;i<fbl;i++ )
			fieldBoosts[i] = fbd;
	}

	{ //msvc6 scope fix
		for ( int32_t i=0;i<fieldInfos->size();i++ )
			fieldLengths[i] = 0;
	} //msvc6 scope fix
	invertDocument(doc);

	// sort postingTable into an array
	Posting** postings = NULL;
	int32_t postingsLength = 0;
	sortPostingTable(postings,postingsLength);

	//DEBUG:
	/*for (int32_t i = 0; i < postingsLength; i++) {
		Posting* posting = postings[i];
		
		TCHAR* b = posting->term->toString();
		_cout << b << " freq=" << posting->freq;
		_CLDELETE(b);

		_cout << " pos=" << posting->positions[0];
		for (int32_t j = 1; j < posting->freq; j++)
			_cout <<"," << posting->positions[j];
		
		_cout << endl;
	}*/


	// write postings
	writePostings(postings,postingsLength, segment);

	// write norms of indexed fields
	writeNorms(segment);
	_CLDELETE_ARRAY( postings );
}

void DocumentWriter::sortPostingTable(Posting**& array, int32_t& arraySize) {
	// copy postingTable into an array
	arraySize = postingTable->size();
	array = _CL_NEWARRAY(Posting*,arraySize);
	PostingTableType::iterator postings = postingTable->begin();
	int32_t i=0;
	while ( postings != postingTable->end() ){
		array[i] = (Posting*)postings->second;
		postings++;
		i++;
	}
	// sort the array
	quickSort(array, 0, i - 1);
}


void DocumentWriter::invertDocument(const Document* doc) {
	DocumentFieldEnumeration* fields = doc->getFields();
	try {
		while (fields->hasMoreElements()) {
			Field* field = (Field*)fields->nextElement();
			const TCHAR* fieldName = field->name();
		    const int32_t fieldNumber = fieldInfos->fieldNumber(fieldName);
		    
		    int32_t length = fieldLengths[fieldNumber];     // length of field
		    int32_t position = fieldPositions[fieldNumber]; // position in field
		    if (length>0) 
		    	position+=analyzer->getPositionIncrementGap(fieldName);
	  		int32_t offset = fieldOffsets[fieldNumber];       // offset field
	  
		    if (field->isIndexed()) {
			    if (!field->isTokenized()) { // un-tokenized field
					//FEATURE: this is bug in java: if using a Reader, then
					//field value will not be added. With CLucene, an untokenized
					//field with a reader will still be added (if it isn't stored,
					//because if it's stored, then the reader has already been read.
					const TCHAR* charBuf = NULL;
					int64_t dataLen = 0;

					if (field->stringValue() == NULL && !field->isStored() ) {
						CL_NS(util)::Reader* r = field->readerValue();
						// this call tries to read the entire stream
						// this may invalidate the string for the further calls
						// it may be better to do this via a FilterReader
						// TODO make a better implementation of this
						dataLen = r->read(charBuf, LUCENE_INT32_MAX_SHOULDBE);
						if (dataLen == -1)
							dataLen = 0;
						//todo: would be better to pass the string length, in case
						//a null char is passed, but then would need to test the output too.
					} else {
						charBuf = field->stringValue();
						dataLen = _tcslen(charBuf);
					}
					
					if(field->isStoreOffsetWithTermVector()){
						TermVectorOffsetInfo tio;
						tio.setStartOffset(offset);
						tio.setEndOffset(offset + dataLen);
						addPosition(fieldName, charBuf, position++, &tio );
					}else
						addPosition(fieldName, charBuf, position++, NULL);
					offset += dataLen;
					length++;
			    } else { // field must be tokenized
			        CL_NS(util)::Reader* reader; // find or make Reader
			        bool delReader = false;
			        if (field->readerValue() != NULL) {
			          reader = field->readerValue();
			        } else if (field->stringValue() != NULL) {
			          reader = _CLNEW CL_NS(util)::StringReader(field->stringValue(),_tcslen(field->stringValue()),false);
			          delReader = true;
			        } else {
			          _CLTHROWA(CL_ERR_IO,"field must have either String or Reader value");
			        }
			
			        try {
			          // Tokenize field and add to postingTable.
			          CL_NS(analysis)::TokenStream* stream = analyzer->tokenStream(fieldName, reader);
			
			          try {
			            CL_NS(analysis)::Token t;
			            int32_t lastTokenEndOffset = -1;
			            while (stream->next(&t)) {
			                position += (t.getPositionIncrement() - 1);
			                
			                if(field->isStoreOffsetWithTermVector()){
			                	TermVectorOffsetInfo tio;
			                	tio.setStartOffset(offset + t.startOffset());
			                	tio.setEndOffset(offset + t.endOffset());
								addPosition(fieldName, t.termBuffer(), position++, &tio);
							}else
								addPosition(fieldName, t.termBuffer(), position++, NULL);
							
							lastTokenEndOffset = t.endOffset();
			                length++;
			                // Apply field truncation policy.
							if (maxFieldLength != IndexWriter::FIELD_TRUNC_POLICY__WARN) {
			                  // The client programmer has explicitly authorized us to
			                  // truncate the token stream after maxFieldLength tokens.
			                  if ( length > maxFieldLength) {
			                    break;
			                  }
							} else if (length > IndexWriter::DEFAULT_MAX_FIELD_LENGTH) {
			                  const TCHAR* errMsgBase = 
			                    _T("Indexing a huge number of tokens from a single")
			                    _T(" field (\"%s\", in this case) can cause CLucene")
			                    _T(" to use memory excessively.")
			                    _T("  By default, CLucene will accept only %s tokens")
			                    _T(" tokens from a single field before forcing the")
			                    _T(" client programmer to specify a threshold at")
			                    _T(" which to truncate the token stream.")
			                    _T("  You should set this threshold via")
								      _T(" IndexReader::maxFieldLength (set to LUCENE_INT32_MAX")
			                    _T(" to disable truncation, or a value to specify maximum number of fields).");
			                  
			                  TCHAR defaultMaxAsChar[34];
			                  _i64tot(IndexWriter::DEFAULT_MAX_FIELD_LENGTH,
			                      defaultMaxAsChar, 10
			                    );
							         int32_t errMsgLen = _tcslen(errMsgBase)
			                      + _tcslen(fieldName)
			                      + _tcslen(defaultMaxAsChar);
			                  TCHAR* errMsg = _CL_NEWARRAY(TCHAR,errMsgLen+1);
			
			                  _sntprintf(errMsg, errMsgLen,errMsgBase, fieldName, defaultMaxAsChar);
			
							  _CLTHROWT_DEL(CL_ERR_Runtime,errMsg);
			                }
			            } // while token->next
						
						if(lastTokenEndOffset != -1 )
              				offset += lastTokenEndOffset + 1;
			          } _CLFINALLY (
			            stream->close();
			            _CLDELETE(stream);
			          );
			        } _CLFINALLY (
			          if (delReader) {
			            _CLDELETE(reader);
			          }
			        );
			    } // if/else field is to be tokenized
				fieldLengths[fieldNumber] = length; // save field length
				fieldPositions[fieldNumber] = position;	  // save field position
				fieldBoosts[fieldNumber] *= field->getBoost();
				fieldOffsets[fieldNumber] = offset;
			} // if field is to beindexed
		} // while more fields available
	} _CLFINALLY (
	  _CLDELETE(fields);
	);
} // Document:;invertDocument


void DocumentWriter::addPosition(const TCHAR* field,
                                     const TCHAR* text,
                                     const int32_t position, 
                                     TermVectorOffsetInfo* offset) {
    
	termBuffer->set(field,text,false);

	Posting* ti = postingTable->get(termBuffer);
	if (ti != NULL) {				  // word seen before
		int32_t freq = ti->freq;
		if (ti->positions.length == freq) {
		    // positions array is full, realloc its size
				ti->positions.length = freq*2;
		    ti->positions.values = (int32_t*)realloc(ti->positions.values, ti->positions.length * sizeof(int32_t));
		}
		ti->positions.values[freq] = position;		  // add new position
		
		if (offset != NULL) {
			if (ti->offsets.length == freq){
				ti->offsets.length = freq*2;
				ti->offsets.values = (TermVectorOffsetInfo*)realloc(ti->offsets.values, ti->offsets.length * sizeof(TermVectorOffsetInfo));
			}
			ti->offsets[freq] = *offset;
		}

		ti->freq = freq + 1;			  // update frequency
	} else {					  // word not seen before
		Term* term = _CLNEW Term( field, text, false);
		postingTable->put(term, _CLNEW Posting(term, position, offset));
	}
}

//static
void DocumentWriter::quickSort(Posting**& postings, const int32_t lo, const int32_t hi) {
	if(lo >= hi)
		return;

	int32_t mid = (lo + hi) / 2;

	if(postings[lo]->term->compareTo(postings[mid]->term) > 0) {
		 Posting* tmp = postings[lo];
		postings[lo] = postings[mid];
		postings[mid] = tmp;
	}

	if(postings[mid]->term->compareTo(postings[hi]->term) > 0) {
		Posting* tmp = postings[mid];
		postings[mid] = postings[hi];
		postings[hi] = tmp;
	      
		if(postings[lo]->term->compareTo(postings[mid]->term) > 0) {
			Posting* tmp2 = postings[lo];
			postings[lo] = postings[mid];
			postings[mid] = tmp2;
		}
	}

	int32_t left = lo + 1;
	int32_t right = hi - 1;

	if (left >= right)
		return; 

	const Term* partition = postings[mid]->term; //not kept, so no need to finalize
    
	for( ;; ) {
		while(postings[right]->term->compareTo(partition) > 0)
		--right;
	      
		while(left < right && postings[left]->term->compareTo(partition) <= 0)
			++left;
		      
		if(left < right) {
			Posting* tmp = postings[left];
			postings[left] = postings[right];
			postings[right] = tmp;
			--right;
		} else {
			break;
		}
	}

	quickSort(postings, lo, left);
	quickSort(postings, left + 1, hi);
}

void DocumentWriter::writePostings(Posting** postings, const int32_t postingsLength, const char* segment){
	#define __DOCLOSE(obj) if(obj!=NULL){ try{ obj->close(); _CLDELETE(obj);} catch(CLuceneError &e){ierr=e.number();err=e.what();} catch(...){err="Unknown error while closing posting tables";} }
	IndexOutput* freq = NULL;
	IndexOutput* prox = NULL;
	TermInfosWriter* tis = NULL;
	TermVectorsWriter* termVectorWriter = NULL;
	try {
		//open files for inverse index storage
	   	const char* buf = Misc::segmentname( segment, ".frq");
		freq = directory->createOutput( buf );
		_CLDELETE_CaARRAY( buf );
		
		buf = Misc::segmentname( segment, ".prx");
		prox = directory->createOutput( buf );
		_CLDELETE_CaARRAY( buf );

		tis = _CLNEW TermInfosWriter(directory, segment, fieldInfos,
			termIndexInterval);
		TermInfo* ti = _CLNEW TermInfo();
		const TCHAR* currentField = NULL;
		for (int32_t i = 0; i < postingsLength; i++) {
			Posting* posting = postings[i];

			// add an entry to the dictionary with pointers to prox and freq files
			ti->set(1, freq->getFilePointer(), prox->getFilePointer(), -1);
			tis->add(posting->term, ti);
			
			// add an entry to the freq file
			int32_t postingFreq = posting->freq;
			if (postingFreq == 1)				  // optimize freq=1
				freq->writeVInt(1);			  // set low bit of doc num.
			else {
				freq->writeVInt(0);			  // the document number
				freq->writeVInt(postingFreq);			  // frequency in doc
			}
			
			int32_t lastPosition = 0;			  // write positions
			for (int32_t j = 0; j < postingFreq; ++j) {		  // use delta-encoding
				prox->writeVInt(posting->positions.values[j] - lastPosition);
				lastPosition = posting->positions.values[j];
			}

	        // check to see if we switched to a new field
	        const TCHAR* termField = posting->term->field();
	        if ( currentField == NULL || _tcscmp(currentField,termField) != 0 ) { //todo, can we do an intern'd check?
	           // changing field - see if there is something to save
	           currentField = termField;
	           FieldInfo* fi = fieldInfos->fieldInfo(currentField);
	           
			   if (fi->storeTermVector) {
	              if (termVectorWriter == NULL) {
	                 termVectorWriter =
	                    _CLNEW TermVectorsWriter(directory, segment, fieldInfos);
	                 termVectorWriter->openDocument();
	              }
	              termVectorWriter->openField(currentField);
	           } else if (termVectorWriter != NULL) {
	              termVectorWriter->closeField();
	           }
	        }
	        if (termVectorWriter != NULL && termVectorWriter->isFieldOpen()) {
	           termVectorWriter->addTerm(posting->term->text(), postingFreq, &posting->positions, &posting->offsets);
	        }
		}
	    if (termVectorWriter != NULL)
	    	termVectorWriter->closeDocument();
		_CLDELETE(ti);
    }_CLFINALLY ( 
        const char* err=NULL;
		int32_t ierr=0;

        // make an effort to close all streams we can but remember and re-throw
        // the first exception encountered in this process
        __DOCLOSE(freq);
        __DOCLOSE(prox);
        __DOCLOSE(tis);
        __DOCLOSE(termVectorWriter);
        if ( err != NULL )
           _CLTHROWA(ierr,err);
    );
}

void DocumentWriter::writeNorms(const char* segment) {
  char fn[CL_MAX_PATH];
  for(int32_t n = 0; n < fieldInfos->size(); n++){
     FieldInfo* fi = fieldInfos->fieldInfo(n);
     if(fi->isIndexed && !fi->omitNorms){
        float_t norm = fieldBoosts[n] * similarity->lengthNorm(fi->name, fieldLengths[n]);

        _snprintf(fn,CL_MAX_PATH,"%s.f%d",segment,n);
        IndexOutput* norms = directory->createOutput(fn);
        try {
           norms->writeByte(CL_NS(search)::Similarity::encodeNorm(norm));
			}_CLFINALLY ( 
			    norms->close();
			    _CLDELETE(norms);
        )
     }
  }
}
CL_NS_END