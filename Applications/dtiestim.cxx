/*=========================================================================

  Program:   NeuroLib (DTI command line tools)
  Language:  C++
  Date:      $Date: 2008-08-19 16:11:15 $
  Version:   $Revision: 1.6 $
  Author:    Casey Goodlett (gcasey@sci.utah.edu)

  Copyright (c)  Casey Goodlett. All rights reserved.
  See NeuroLibCopyright.txt or http://www.ia.unc.edu/dev/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
// This program estimates a single diffusion tensor model at every
// voxel in an image.

// STL includes
#include <string>
#include <iostream>

// boost includes
#include <boost/program_options/option.hpp>
#include <boost/program_options/positional_options.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/cmdline.hpp>

// ITK includes
// datastructures
#include <itkImage.h>
#include <itkVector.h>
#include <itkMetaDataObject.h>
#include <itkVersion.h>

// IO
#include <itkImageFileReader.h>
#include <itkImageFileWriter.h>

// Filters
#include <itkTensorFractionalAnisotropyImageFilter.h>
#include <itkShiftScaleImageFilter.h>
#include <itkVectorIndexSelectionCastImageFilter.h>
#include <itkBinaryThresholdImageFilter.h>
#include <itkLogImageFilter.h>
#include <itkAddImageFilter.h>
#include <itkExpImageFilter.h>
#include <itkImageRegionIterator.h>

#include <itkNthElementImageAdaptor.h>
#include <itkOtsuThresholdImageCalculator.h>

#include "itkVectorMaskNegatedImageFilter.h"
#include "itkVectorMaskImageFilter.h"
#include "itkDiffusionTensor3DReconstructionNonlinearImageFilter.h"
#include "itkDiffusionTensor3DReconstructionWeightedImageFilter.h"
#include "itkDiffusionTensor3DReconstructionRicianImageFilter.h"
#include "itkDiffusionTensor3DReconstructionLinearImageFilter.h"
#include "itkTensorRotateImageFilter.h"

#include <vnl/algo/vnl_svd.h>

#include "dtitypes.h"

const char* NRRD_MEASUREMENT_KEY = "NRRD_measurement frame";

enum EstimationType {LinearEstimate, NonlinearEstimate, WeightedEstimate, MaximumLikelihoodEstimate};

void validate(boost::any& v,
              const std::vector<std::string>& values,
              EstimationType* target_type,
              int)
{
  using namespace boost::program_options;
  using boost::any;

  // Make sure no previous assignment to 'a' was made.
  validators::check_first_occurrence(v);
  // Extract the first string from 'values'. If there is more than
  // one string, it's an error, and exception will be thrown.
  const std::string& s = validators::get_single_string(values);

  if(s == "lls" || s == "linear")
  {
    v = any(LinearEstimate);
  }
  else if (s == "nls" || s == "nonlinear")
  {
    v = any(NonlinearEstimate);
  }
  else if (s == "wls" || s == "weighted")
  {
    v = any(WeightedEstimate);
  }
  else if (s == "ml")
  {
    v = any(MaximumLikelihoodEstimate);
  }
  else
  {
    throw validation_error("Estimation type invalid.  Only \"lls\", \"nls\", \"wls\", and \"ml\" allowed.");
  }
}

int main(int argc, char* argv[])
{
  namespace po = boost::program_options;


//  unsigned int scale;

  // Read program options/configuration
  po::options_description config("Usage: dtiestim dwi-image tensor-output [options]");
  config.add_options()
    ("help,h", "produce this help message")

    ("brain-mask,M", po::value<std::string>(),
     "Brain mask.  Image where for every voxel == 0 the tensors are not estimated.")
    ("bad-region-mask,B", po::value<std::string>(),
     "Bad region mask.  Image where for every voxel > 0 the tensors are not estimated.")
    ("threshold,t", po::value<ScalarPixelType>(),
     "Baseline threshold for estimation.  If not specified calculated using an OTSU threshold on the baseline image.")
    ("idwi", po::value<std::string>(),
     "idwi output image.  Image with isotropic diffusion-weighted information = geometric mean of diffusion images.")

    ("method,m", po::value<EstimationType>()->default_value(LinearEstimate,"lls (Linear Least Squares)"),
     "Estimation method (lls,wls,nls,ml)")

    // WLS options
    ("weight-iterations", po::value<unsigned int>()->default_value(1),
     "Number of iterations to recaluate weightings from tensor estimate")

    // Optimization options
    ("step,s", po::value<double>()->default_value(1.0e-8),
     "Gradient descent step size (for nls and ml methods)")
    ("sigma", po::value<double>(),
     "Sigma parameter for Rician ML estimation (Std deviation of Gaussian noise in k-space).")

    ("verbose,v",
     "Verbose output")
    ;

  po::options_description hidden("Hidden options");
  hidden.add_options()
    ("dwi-image", po::value<std::string>(), "DWI image volume.")
    ("tensor-output", po::value<std::string>(), "Tensor output.")
    ;

  po::options_description all;
  all.add(config).add(hidden);

  po::positional_options_description p;
  p.add("dwi-image",1);
  p.add("tensor-output",1);

  po::variables_map vm;

  try
  {
    po::store(po::command_line_parser(argc, argv).
              options(all).positional(p).run(), vm);
    po::notify(vm);     
  } 
  catch (const po::error &e)
  {
    std::cout << config << std::endl;
    return EXIT_FAILURE;
  }

  // End option reading configuration

  // Display help if asked or program improperly called
  if(vm.count("help") || !vm.count("dwi-image") || !vm.count("tensor-output"))
  {
    std::cout << config << std::endl;
    if(vm.count("help"))
    {
      std::cout << "Version: $Date: 2008-08-19 16:11:15 $ $Revision: 1.6 $" << std::endl;
      std::cout << ITK_SOURCE_VERSION << std::endl;
      return EXIT_SUCCESS;
    }
    else
    {
      std::cerr << "DWI image and output tensor filename needs to be specified." << std::endl;
      return EXIT_FAILURE;
    }
  }

  bool VERBOSE = false;
  if(vm.count("verbose"))
    VERBOSE = true;

  double step = 1.0e-8, sigma = 0.0;
  try
  {
    step = vm["step"].as<double>();
  }
  catch( ... )
  {
    if(vm["method"].as<EstimationType>() == NonlinearEstimate || vm["method"].as<EstimationType>() == MaximumLikelihoodEstimate)
    {
      std::cerr << "Step size not set for optimization method" << std::endl;
      return EXIT_FAILURE;
    }    
  }
  try
  {
    sigma = vm["sigma"].as<double>();
  }
  catch( ... )
  {
    if(vm["method"].as<EstimationType>() == MaximumLikelihoodEstimate)
    {
      std::cerr << "Noise level not set for optimization method" << std::endl;
      return EXIT_FAILURE;
    }    
    
  }

  // Read diffusion weighted MR

  typedef itk::ImageFileReader<VectorImageType> FileReaderType;
  FileReaderType::Pointer dwireader = FileReaderType::New();
  dwireader->SetFileName(vm["dwi-image"].as<std::string>().c_str());

  try
  {
    if(VERBOSE)
      std::cout << "Reading Data" << std::endl;
    dwireader->Update();
  }
  catch (itk::ExceptionObject & e)
  {
    std::cerr << e <<std::endl;
    return EXIT_FAILURE;
  }

  VectorImageType::Pointer dwi = dwireader->GetOutput();

  // Read dwi meta-data

  // read into b0 the DWMRI_b-value that is the b-value of
  // the experiment
  double b0 = 0;
  bool readbvalue = false;

  // read into gradientContainer the gradients
  typedef itk::DiffusionTensor3DReconstructionLinearImageFilter<ScalarPixelType, RealType>
    DiffusionEstimationFilterType;
  typedef itk::DiffusionTensor3DReconstructionNonlinearImageFilter<ScalarPixelType, RealType>
    NLDiffusionEstimationFilterType;
  typedef itk::DiffusionTensor3DReconstructionRicianImageFilter<ScalarPixelType,ScalarPixelType, RealType>
    MLDiffusionEstimationFilterType;
  typedef itk::DiffusionTensor3DReconstructionWeightedImageFilter<ScalarPixelType, RealType>
    WLDiffusionEstimationFilterType;


  DiffusionEstimationFilterType::GradientDirectionContainerType::Pointer gradientContainer = 
    DiffusionEstimationFilterType::GradientDirectionContainerType::New();
  
  typedef DiffusionEstimationFilterType::GradientDirectionType GradientType;

  itk::MetaDataDictionary & dict = dwi->GetMetaDataDictionary();

  vnl_matrix<double> transform(3,3);
  transform.set_identity();

  // Apply measurement frame if it exists
  if(dict.HasKey(NRRD_MEASUREMENT_KEY))
  {
    if(VERBOSE)
      std::cout << "Reorienting gradient directions to image coordinate frame" << std::endl;

    // measurement frame
    vnl_matrix<double> mf(3,3);
    // imaging frame
    vnl_matrix<double> imgf(3,3);
    std::vector<std::vector<double> > nrrdmf;
    itk::ExposeMetaData<std::vector<std::vector<double> > >(dict,NRRD_MEASUREMENT_KEY,nrrdmf);

    imgf = dwi->GetDirection().GetVnlMatrix();
    if(VERBOSE)
    {
      std::cout << "Image frame: " << std::endl;
      std::cout << imgf << std::endl;
    }

    for(unsigned int i = 0; i < 3; ++i)
    {
      for(unsigned int j = 0; j < 3; ++j)
      {
        mf(i,j) = nrrdmf[j][i];

        nrrdmf[j][i] = imgf(i,j);
      }
    }

    if(VERBOSE)
    {
      std::cout << "Meausurement frame: " << std::endl;
      std::cout << mf << std::endl;
    }

    itk::EncapsulateMetaData<std::vector<std::vector<double> > >(dict,NRRD_MEASUREMENT_KEY,nrrdmf);
    // prevent slicer error

    transform = vnl_svd<double>(imgf).inverse()*mf;

    if(VERBOSE)
    {
      std::cout << "Transform: " << std::endl;
      std::cout << transform << std::endl;
    }

  }

  if(dict.HasKey("modality"))
  {
    itk::EncapsulateMetaData<std::string>(dict,"modality","DTMRI");
  }

  std::vector<std::string> keys = dict.GetKeys();
  for(std::vector<std::string>::const_iterator it = keys.begin();
      it != keys.end(); ++it)
  {
    if( it->find("DWMRI_b-value") != std::string::npos)
    {
      std::string t;
      itk::ExposeMetaData<std::string>(dict, *it, t);
      readbvalue = true;
      b0 = atof(t.c_str());
    }
    else if( it->find("DWMRI_gradient") != std::string::npos)
    {
      std::string value;

      itk::ExposeMetaData<std::string>(dict, *it, value);
      std::istringstream iss(value);
      GradientType g;
      iss >> g[0] >> g[1] >> g[2];

      g = transform * g;

      unsigned int ind;
      std::string temp = it->substr(it->find_last_of('_')+1);
      ind = atoi(temp.c_str());
      
      gradientContainer->InsertElement(ind,g);
    }
  }
  for(std::vector<std::string>::const_iterator it = keys.begin();
      it != keys.end(); ++it)
  {
    if( it->find("DWMRI_NEX") != std::string::npos)
    {
      std::string numrepstr;

      itk::ExposeMetaData<std::string>(dict, *it, numrepstr);
      unsigned int numreps = atoi(numrepstr.c_str());

      std::string indtorepstr = it->substr(it->find_last_of('_')+1);
      unsigned int indtorep =  atoi(indtorepstr.c_str());

      GradientType g = gradientContainer->GetElement(indtorep);

      for(unsigned int i = indtorep+1; i < indtorep+numreps; i++)
        gradientContainer->InsertElement(i,g);
    }

  }

  if(VERBOSE)
  {
    std::cout << "NGrads: " << gradientContainer->Size() << std::endl;
    for(unsigned int i = 0; i < gradientContainer->Size(); ++i)
    {
      std::cout << gradientContainer->GetElement(i) << std::endl;
    }
  }

  if(!readbvalue)
  {
    std::cerr << "BValue not specified in header file" << std::endl;
    return EXIT_FAILURE;
  }

  if(VERBOSE)
    std::cout << "BValue: " << b0 << std::endl;

  // Read brain mask if it is specified.  
  if(vm.count("brain-mask"))
  {
    typedef itk::ImageFileReader<LabelImageType> MaskFileReaderType;
    MaskFileReaderType::Pointer maskreader = MaskFileReaderType::New();
    maskreader->SetFileName(vm["brain-mask"].as<std::string>().c_str());

    try
    {
      maskreader->Update();
      
      if(VERBOSE)
        std::cout << "Masking Data" << std::endl;

      typedef itk::VectorMaskImageFilter<VectorImageType,LabelImageType,VectorImageType> MaskFilterType;
      MaskFilterType::Pointer mask = MaskFilterType::New();
//      mask->ReleaseDataFlagOn();
      mask->SetInput1(dwireader->GetOutput());
      mask->SetInput2(maskreader->GetOutput());
      mask->Update();

      dwi = mask->GetOutput();
    }
    catch (itk::ExceptionObject & e)
    {
      std::cerr << e <<std::endl;
      return EXIT_FAILURE;
    }

  }
  
  // Read negative mask
  if(vm.count("bad-region-mask"))
  {
    typedef itk::ImageFileReader<LabelImageType> MaskFileReaderType;
    MaskFileReaderType::Pointer maskreader = MaskFileReaderType::New();

    
    //  Go ahead and read data so we can use adaptors as necessary
    try
    {
      if(VERBOSE)
        std::cout << "Masking Bad Regions" << std::endl;

      maskreader->Update();
      
      typedef itk::VectorMaskNegatedImageFilter<VectorImageType,LabelImageType,VectorImageType> MaskFilterType;
      MaskFilterType::Pointer mask = MaskFilterType::New();
//      mask->ReleaseDataFlagOn();
      mask->SetInput1(dwi);
      mask->SetInput2(maskreader->GetOutput());
      mask->Update();

      dwi = mask->GetOutput();
    }
    catch (itk::ExceptionObject & e)
    {
      std::cerr << e <<std::endl;
      return EXIT_FAILURE;
    }
  }

  // If we didnt specify a threshold compute it as the ostu threshold
  // of the baseline image
  

  ScalarPixelType threshold;
  if(vm.count("threshold"))
  {
    threshold = vm["threshold"].as<ScalarPixelType>();
  }
  else
  {
    typedef itk::VectorIndexSelectionCastImageFilter<VectorImageType, IntImageType>
      BaselineExtractAdaptorType;

    BaselineExtractAdaptorType::Pointer baselineextract = BaselineExtractAdaptorType::New();
    baselineextract->SetInput(dwireader->GetOutput());
    baselineextract->SetIndex(0);
    baselineextract->Update();

    typedef itk::OtsuThresholdImageCalculator<IntImageType> 
      OtsuThresholdCalculatorType;

    OtsuThresholdCalculatorType::Pointer  otsucalculator = OtsuThresholdCalculatorType::New();
    otsucalculator->SetImage(baselineextract->GetOutput());
    otsucalculator->Compute();
    threshold = static_cast<ScalarPixelType>(.9 * otsucalculator->GetThreshold());

    if(VERBOSE)
      std::cout << "Otsu threshold: " << threshold << std::endl;
  }

  // Output b0 threshold mask if requested
  if(vm.count("threshold-mask"))
  {
    // Will take last B0 image in sequence

    typedef itk::VectorIndexSelectionCastImageFilter<VectorImageType,IntImageType> VectorSelectionFilterType;
    VectorSelectionFilterType::Pointer b0extract = VectorSelectionFilterType::New();
    b0extract->SetInput(dwi);
    for (unsigned int directionIndex = 0 ; directionIndex < gradientContainer->Size(); directionIndex++)
    {
      // information whether image is b0 or not
      GradientType g = gradientContainer->GetElement(directionIndex);
      if (g[0] == 0 && g[1] == 0 && g[2] == 0) 
      {
	b0extract->SetIndex(directionIndex);
      }
    }

    typedef itk::BinaryThresholdImageFilter<IntImageType,LabelImageType> ThresholdFilterType;
    ThresholdFilterType::Pointer thresholdfilter = ThresholdFilterType::New();
    thresholdfilter->SetInput(b0extract->GetOutput());
    thresholdfilter->SetLowerThreshold(threshold);
    thresholdfilter->SetUpperThreshold(itk::NumericTraits<ScalarPixelType>::max());
    thresholdfilter->Update();

    try 
    {
      typedef itk::ImageFileWriter<LabelImageType> MaskImageFileWriterType;
      MaskImageFileWriterType::Pointer maskwriter = MaskImageFileWriterType::New();
      maskwriter->SetInput(thresholdfilter->GetOutput());
      maskwriter->SetFileName(vm["threshold-mask"].as<std::string>().c_str());
      maskwriter->Update();
    }
    catch (itk::ExceptionObject & e)
    {
      std::cerr << "Could not write threshold mask file" << std::endl;
      std::cerr << e << std::endl;
    }

  }
  
  // Output idwi image if requested
  if(vm.count("idwi"))
  { 
    typedef itk::VectorIndexSelectionCastImageFilter<VectorImageType,RealImageType> VectorSelectionFilterType;
    VectorSelectionFilterType::Pointer biextract = VectorSelectionFilterType::New();
    biextract->SetInput(dwi);
    int numberNonB0Directions = 0;

    RealImageType::Pointer idwiImage; 

    typedef itk::LogImageFilter<RealImageType,RealImageType> LogImageFilterType;

    for (unsigned int directionIndex = 0 ; directionIndex < gradientContainer->Size(); directionIndex++)
    {
      // get information whether image is b0 or not
      GradientType g = gradientContainer->GetElement(directionIndex);
      if ( g[0] != 0 || g[1] != 0 || g[2] != 0 ) 
      {
	// image is not b0 image
	biextract->SetIndex(directionIndex);

	if (numberNonB0Directions == 0) 
	{
	  // log of the first image and set it as current idwi image
	  try 
	  {
	    LogImageFilterType::Pointer logfilter = LogImageFilterType::New();
	    logfilter->SetInput(biextract->GetOutput());
	    logfilter->Update();
	    idwiImage = logfilter->GetOutput();
	  }
	  catch (itk::ExceptionObject & e)
	  {
	    std::cerr << "Error in log computation" << std::endl;
	    std::cerr << e << std::endl;
	  }

	} 
	else 
	{
	  // log of the image and add it to the current idwi image
	  try 
	  {
	    LogImageFilterType::Pointer logfilter = LogImageFilterType::New();
	    logfilter->SetInput(biextract->GetOutput());
	    logfilter->Update();
	    typedef itk::AddImageFilter<RealImageType> AddImageFilterType;
	    AddImageFilterType::Pointer addfilter = AddImageFilterType::New();
	    addfilter->SetInput1(logfilter->GetOutput());
	    addfilter->SetInput2(idwiImage);
	    addfilter->Update();
	    idwiImage = addfilter->GetOutput();
	  }
	  catch (itk::ExceptionObject & e)
	  {
	    std::cerr << "Error in log computation" << std::endl;
	    std::cerr << e << std::endl;
	  }
	  
	}
	
	numberNonB0Directions++;
      }
    }

    // idwiImage contains the sum of all log transformed directional images
    // need to divide by numberNonB0Directions and compute exponential image

    if(VERBOSE)
      std::cout << "Number of non B0 images : " << numberNonB0Directions << std::endl;

    typedef itk::ImageRegionIterator< RealImageType > RealIterator;
    RealIterator iterImage (idwiImage, idwiImage->GetBufferedRegion());
    while ( !iterImage.IsAtEnd() )  {
      iterImage.Set(iterImage.Get() / numberNonB0Directions);
      ++iterImage;
    }

    typedef itk::ExpImageFilter<RealImageType,RealImageType> ExpFilterType;
    ExpFilterType::Pointer expfilter = ExpFilterType::New();
    expfilter->SetInput(idwiImage);

    try 
    {
      typedef itk::ImageFileWriter<RealImageType> RealImageFileWriterType;
      RealImageFileWriterType::Pointer realwriter = RealImageFileWriterType::New();
      realwriter->SetInput(expfilter->GetOutput());
      realwriter->SetFileName(vm["idwi"].as<std::string>().c_str());
      realwriter->Update();
    }
    catch (itk::ExceptionObject & e)
    {
      std::cerr << "Could not write idwi file" << std::endl;
      std::cerr << e << std::endl;
    }

  }
  
  // Estimate tensors
  typedef itk::ImageToImageFilter<VectorImageType, TensorImageType> DiffusionEstimationBaseType;
  TensorImageType::Pointer tensors;

  if(VERBOSE)
  {
    std::cout << "Estimation method: " << vm["method"].as<EstimationType>() << std::endl;
  }

  DiffusionEstimationFilterType::Pointer llsestimator = DiffusionEstimationFilterType::New();
  llsestimator->ReleaseDataFlagOn();
  llsestimator->SetGradientImage(gradientContainer,dwi);
  llsestimator->SetBValue(b0);
  llsestimator->SetThreshold(threshold);
  llsestimator->Update();
  tensors = llsestimator->GetOutput();

  if(vm["method"].as<EstimationType>() == LinearEstimate)
  {
  }
  else if(vm["method"].as<EstimationType>() == NonlinearEstimate)
  {
    NLDiffusionEstimationFilterType::Pointer estimator = NLDiffusionEstimationFilterType::New();
    estimator->ReleaseDataFlagOn();
    
    TensorImageType::Pointer llstensors = tensors;

    estimator->SetGradientImage(gradientContainer,dwi);
    estimator->SetBValue(b0);
    estimator->SetThreshold(threshold);
    estimator->SetStep(step);
    estimator->SetNumberOfThreads(1);
    estimator->Update();
    tensors = estimator->GetOutput();
  }
  else if(vm["method"].as<EstimationType>() == WeightedEstimate)
  {
    WLDiffusionEstimationFilterType::Pointer estimator = WLDiffusionEstimationFilterType::New();
    estimator->ReleaseDataFlagOn();

    TensorImageType::Pointer llstensors = tensors;

    if(VERBOSE)
      std::cout << "Weighting steps: " << vm["weight-iterations"].as<unsigned int>() << std::endl;

    estimator->SetGradientImage(gradientContainer,dwi);
    estimator->SetBValue(b0);
    estimator->SetThreshold(threshold);
    estimator->SetNumberOfIterations(vm["weight-iterations"].as<unsigned int>());
    estimator->Update();
    tensors = estimator->GetOutput();
  }
  else if(vm["method"].as<EstimationType>() == MaximumLikelihoodEstimate)
  {
    MLDiffusionEstimationFilterType::Pointer estimator = MLDiffusionEstimationFilterType::New();
    estimator->ReleaseDataFlagOn();

    TensorImageType::Pointer llstensors = tensors;

    estimator->SetGradientImage(gradientContainer,dwi);
    estimator->SetBValue(b0);
    estimator->SetThreshold(threshold);
    estimator->SetInitialTensor(llstensors);
    estimator->SetStep(step);
    estimator->SetNumberOfThreads(1);
    std::cout << "Start sigma: " << sigma << std::endl;
    estimator->SetSigma(sigma);
    estimator->Update();
    tensors = estimator->GetOutput();
  }
  else
  {
    std::cerr << "Invalid estimation method"  << std::endl;
    return EXIT_FAILURE;
  }
      
  // wp = D*x
  // wv = M*x
  // wv' = D'M*x

  // Write tensor file if requested
  try
  {
    typedef itk::ImageFileWriter<TensorImageType> TensorFileWriterType;

    TensorFileWriterType::Pointer tensorWriter = TensorFileWriterType::New();
    tensorWriter->SetFileName(vm["tensor-output"].as<std::string>().c_str());
    tensors->SetMetaDataDictionary(dict);
    tensorWriter->SetInput(tensors);
    tensorWriter->SetUseCompression(true);
    tensorWriter->Update();
       
  } 
  catch (itk::ExceptionObject e) 
  {
    std::cerr << e << std::endl;
    return EXIT_FAILURE;
  }   
       
  return EXIT_SUCCESS;
}
