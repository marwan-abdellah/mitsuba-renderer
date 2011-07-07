/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2011 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/core/fresolver.h>
#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/sampler.h>
#include <mitsuba/render/texture.h>
#include <mitsuba/hw/gpuprogram.h>
#include "microfacet.h"

MTS_NAMESPACE_BEGIN

/* Suggestion by Bruce Walter: sample the model using a slightly 
   wider density function. This in practice limits the importance 
   weights to values <= 4. 

   Turned off by default, since it seems to increase the variance
   of the reflection component.
*/
#define ENLARGE_LOBE_TRICK 0

/*!\plugin{roughconductor}{Rough conductor material}
 * \order{6}
 * \parameters{
 *     \parameter{distribution}{\String}{
 *          Specifies the type of microfacet normal distribution 
 *          used to model the surface roughness.
 *       \begin{enumerate}[(i)]
 *           \item \code{beckmann}: Physically-based distribution derived from
 *               Gaussian random surfaces. This is the default.
 *           \item \code{ggx}: New distribution proposed by
 *              Walter et al. \cite{Walter07Microfacet}, which is meant to better handle 
 *              the long tails observed in measurements of ground surfaces. 
 *              Renderings with this distribution may converge slowly.
 *           \item \code{phong}: Classical $\cos^p\theta$ distribution.
 *              Due to the underlying microfacet theory, 
 *              the use of this distribution here leads to more realistic 
 *              behavior than the separately available \pluginref{phong} plugin.
 *           \item \code{as}: Anisotropic Phong-style microfacet distribution proposed by
 *              Ashikhmin and Shirley \cite{Ashikhmin2005Anisotropic}.\vspace{-3mm}
 *       \end{enumerate}
 *     }
 *     \parameter{alpha}{\Float\Or\Texture}{
 *         Specifies the roughness of the unresolved surface microgeometry. 
 *         When the Beckmann distribution is used, this parameter is equal to the 
 *         \emph{root mean square} (RMS) slope of the microfacets. This
 *         parameter is only valid when \texttt{distribution=beckmann/phong/ggx}.
 *         \default{0.1}. 
 *     }
 *     \parameter{alphaU, alphaV}{\Float\Or\Texture}{
 *         Specifies the anisotropic rougness values along the tangent and 
 *         bitangent directions. These parameter are only valid when 
 *         \texttt{distribution=as}. \default{0.1}. 
 *     }
 *     \parameter{preset}{\String}{Name of a material preset, see 
 *           \tblref{conductor-iors}.\!\default{\texttt{Cu} / copper}}
 *     \parameter{eta}{\Spectrum}{Real part of the material's index 
 *           of refraction \default{based on the value of \texttt{preset}}}
 *     \parameter{k}{\Spectrum}{Imaginary part of the material's index of 
 *             refraction, also known as absorption coefficient.
 *             \default{based on the value of \texttt{preset}}}
 *     \lastparameter{specular\showbreak Reflectance}{\Spectrum\Or\Texture}{Optional
 *         factor used to modulate the reflectance component\default{1.0}}
 * }
 * \renderings{
 *     \rendering{Rough copper (Beckmann, $\alpha=0.1$)}
 *     	   {bsdf_roughconductor_copper.jpg}
 *     \rendering{Vertically brushed aluminium (Ashikhmin-Shirley, 
 *         $\alpha_u=0.05,\
 *         \alpha_v=0.3$)}{bsdf_roughconductor_anisotropic_aluminium.jpg}
 * }
 *
 */
class RoughConductor : public BSDF {
public:
	RoughConductor(const Properties &props) : BSDF(props) {
		ref<FileResolver> fResolver = Thread::getThread()->getFileResolver();

		m_specularReflectance = new ConstantSpectrumTexture(
			props.getSpectrum("specularReflectance", Spectrum(1.0f)));

		std::string preset = props.getString("preset", "Cu");
		Spectrum presetEta, presetK;
		presetEta.fromContinuousSpectrum(InterpolatedSpectrum(
			fResolver->resolve("data/ior/" + preset + ".eta.spd")));
		presetK.fromContinuousSpectrum(InterpolatedSpectrum(
			fResolver->resolve("data/ior/" + preset + ".k.spd")));

		m_eta = props.getSpectrum("eta", presetEta);
		m_k = props.getSpectrum("k", presetK);

		m_distribution = MicrofacetDistribution(
			props.getString("distribution", "beckmann")
		);

		Float alpha = props.getFloat("alpha", 0.1f),
			  alphaU = props.getFloat("alphaU", alpha),
			  alphaV = props.getFloat("alphaV", alpha);

		m_alphaU = new ConstantFloatTexture(alphaU);
		if (alphaU == alphaV)
			m_alphaV = m_alphaU;
		else
			m_alphaV = new ConstantFloatTexture(alphaV);

		m_usesRayDifferentials = false;
	}

	RoughConductor(Stream *stream, InstanceManager *manager) 
	 : BSDF(stream, manager) {
		m_distribution = MicrofacetDistribution(
			(MicrofacetDistribution::EType) stream->readUInt()
		);
		m_alphaU = static_cast<Texture *>(manager->getInstance(stream));
		m_alphaV = static_cast<Texture *>(manager->getInstance(stream));
		m_specularReflectance = static_cast<Texture *>(manager->getInstance(stream));
		m_eta = Spectrum(stream);
		m_k = Spectrum(stream);

		m_usesRayDifferentials = 
			m_alphaU->usesRayDifferentials() ||
			m_alphaV->usesRayDifferentials() ||
			m_specularReflectance->usesRayDifferentials();

		configure();
	}

	void configure() {
		unsigned int extraFlags = 0;
		if (m_alphaU != m_alphaV) {
			extraFlags |= EAnisotropic;
			if (m_distribution.getType() != 
				MicrofacetDistribution::EAshikhminShirley)
				Log(EError, "Different roughness values along the tangent and "
						"bitangent directions are only supported when using the "
						"anisotropic Ashikhmin-Shirley microfacet distribution "
						"(named \"as\")");
		}

		m_components.clear();
		m_components.push_back(
			EGlossyReflection | EFrontSide | extraFlags);

		/* Verify the input parameter and fix them if necessary */
		m_specularReflectance = ensureEnergyConservation(
			m_specularReflectance, "specularReflectance", 1.0f);

		BSDF::configure();
	}

	virtual ~RoughConductor() { }

	/// Helper function: reflect \c wi with respect to a given surface normal
	inline Vector reflect(const Vector &wi, const Normal &m) const {
		return 2 * dot(wi, m) * Vector(m) - wi;
	}

	Spectrum eval(const BSDFQueryRecord &bRec, EMeasure measure) const {
		/* Stop if this component was not requested */
		if (measure != ESolidAngle ||
			Frame::cosTheta(bRec.wi) < 0 ||
			Frame::cosTheta(bRec.wo) < 0 ||
			((bRec.component != -1 && bRec.component != 0) ||
			!(bRec.typeMask & EGlossyReflection)))
			return Spectrum(0.0f);

		/* Calculate the reflection half-vector */
		Vector H = normalize(bRec.wo+bRec.wi); 

		/* Evaluate the roughness */
		Float alphaU = m_distribution.transformRoughness( 
					m_alphaU->getValue(bRec.its).average()),
			  alphaV = m_distribution.transformRoughness( 
					m_alphaV->getValue(bRec.its).average());

		/* Evaluate the microsurface normal distribution */
		const Float D = m_distribution.eval(H, alphaU, alphaV);
		if (D == 0)
			return Spectrum(0.0f);

		/* Fresnel factor */
		const Spectrum F = fresnelConductor(Frame::cosTheta(bRec.wi), m_eta, m_k);

		/* Smith's shadow-masking function */
		const Float G = m_distribution.G(bRec.wi, bRec.wo, H, alphaU, alphaV);

		/* Calculate the total amount of reflection */
		Float value = D * G / (4.0f * Frame::cosTheta(bRec.wi));

		return m_specularReflectance->getValue(bRec.its) * F * value; 
	}

	Float pdf(const BSDFQueryRecord &bRec, EMeasure measure) const {
		if (measure != ESolidAngle ||
			Frame::cosTheta(bRec.wi) < 0 ||
			Frame::cosTheta(bRec.wo) < 0 ||
			((bRec.component != -1 && bRec.component != 0) ||
			!(bRec.typeMask & EGlossyReflection)))
			return 0.0f;

		/* Calculate the reflection half-vector */
		Vector H = normalize(bRec.wo+bRec.wi);
	
		/* Evaluate the roughness */
		Float alphaU = m_distribution.transformRoughness( 
					m_alphaU->getValue(bRec.its).average()),
			  alphaV = m_distribution.transformRoughness( 
					m_alphaV->getValue(bRec.its).average());

#if ENLARGE_LOBE_TRICK == 1
		Float factor = (1.2f - 0.2f * std::sqrt(
			std::abs(Frame::cosTheta(bRec.wi))));
		alphaU *= factor; alphaV *= factor;
#endif

		return m_distribution.pdf(H, alphaU, alphaV)
			/ (4 * absDot(bRec.wo, H));
	}

	Spectrum sample(BSDFQueryRecord &bRec, const Point2 &sample) const {
		if (Frame::cosTheta(bRec.wi) < 0 ||
			((bRec.component != -1 && bRec.component != 0) ||
			!(bRec.typeMask & EGlossyReflection)))
			return Spectrum(0.0f);

		/* Evaluate the roughness */
		Float alphaU = m_distribution.transformRoughness( 
					m_alphaU->getValue(bRec.its).average()),
			  alphaV = m_distribution.transformRoughness( 
					m_alphaV->getValue(bRec.its).average());

#if ENLARGE_LOBE_TRICK == 1
		Float factor = (1.2f - 0.2f * std::sqrt(
			std::abs(Frame::cosTheta(bRec.wi))));
		Float sampleAlphaU = alphaU * factor,
			  sampleAlphaV = alphaV * factor;
#else
		Float sampleAlphaU = alphaU,
			  sampleAlphaV = alphaV;
#endif

		/* Sample M, the microsurface normal */
		const Normal m = m_distribution.sample(sample,
				sampleAlphaU, sampleAlphaV);

		/* Perfect specular reflection based on the microsurface normal */
		bRec.wo = reflect(bRec.wi, m);
		bRec.sampledComponent = 0;
		bRec.sampledType = EGlossyReflection;

		/* Side check */
		if (Frame::cosTheta(bRec.wo) <= 0)
			return Spectrum(0.0f);

		const Spectrum F = fresnelConductor(Frame::cosTheta(bRec.wi),
				m_eta, m_k);

		Float numerator = m_distribution.eval(m, alphaU, alphaV)
			* m_distribution.G(bRec.wi, bRec.wo, m, alphaU, alphaV)
			* dot(bRec.wi, m);

		Float denominator = m_distribution.pdf(m, sampleAlphaU, sampleAlphaV)
			* Frame::cosTheta(bRec.wi);

		return m_specularReflectance->getValue(bRec.its) * F
				* (numerator / denominator);
	}

	Spectrum sample(BSDFQueryRecord &bRec, Float &_pdf, const Point2 &sample) const {
		if (Frame::cosTheta(bRec.wi) < 0 ||
			((bRec.component != -1 && bRec.component != 0) ||
			!(bRec.typeMask & EGlossyReflection)))
			return Spectrum(0.0f);

		/* Evaluate the roughness */
		Float alphaU = m_distribution.transformRoughness( 
					m_alphaU->getValue(bRec.its).average()),
			  alphaV = m_distribution.transformRoughness( 
					m_alphaV->getValue(bRec.its).average());

#if ENLARGE_LOBE_TRICK == 1
		Float factor = (1.2f - 0.2f * std::sqrt(
			std::abs(Frame::cosTheta(bRec.wi))));
		Float sampleAlphaU = alphaU * factor,
			  sampleAlphaV = alphaV * factor;
#else
		Float sampleAlphaU = alphaU,
			  sampleAlphaV = alphaV;
#endif

		/* Sample M, the microsurface normal */
		const Normal m = m_distribution.sample(sample,
				sampleAlphaU, sampleAlphaV);

		/* Perfect specular reflection based on the microsurface normal */
		bRec.wo = reflect(bRec.wi, m);
		bRec.sampledComponent = 0;
		bRec.sampledType = EGlossyReflection;

		/* Side check */
		if (Frame::cosTheta(bRec.wo) <= 0)
			return Spectrum(0.0f);

		/* Guard against numerical imprecisions */
		_pdf = pdf(bRec, ESolidAngle);

		if (_pdf == 0) 
			return Spectrum(0.0f);
		else
			return eval(bRec, ESolidAngle);
	}

	void addChild(const std::string &name, ConfigurableObject *child) {
		if (child->getClass()->derivesFrom(MTS_CLASS(Texture)) && name == "alpha") {
			m_alphaU = m_alphaV = static_cast<Texture *>(child);
			m_usesRayDifferentials |= m_alphaU->usesRayDifferentials();
		} else if (child->getClass()->derivesFrom(MTS_CLASS(Texture)) && name == "alphaU") {
			m_alphaU = static_cast<Texture *>(child);
			m_usesRayDifferentials |= m_alphaU->usesRayDifferentials();
		} else if (child->getClass()->derivesFrom(MTS_CLASS(Texture)) && name == "alphaV") {
			m_alphaV = static_cast<Texture *>(child);
			m_usesRayDifferentials |= m_alphaV->usesRayDifferentials();
		} else if (child->getClass()->derivesFrom(MTS_CLASS(Texture)) && name == "specularReflectance") {
			m_specularReflectance = static_cast<Texture *>(child);
			m_usesRayDifferentials |= m_specularReflectance->usesRayDifferentials();
		} else {
			BSDF::addChild(name, child);
		}
	}

	void serialize(Stream *stream, InstanceManager *manager) const {
		BSDF::serialize(stream, manager);

		stream->writeUInt((uint32_t) m_distribution.getType());
		manager->serialize(stream, m_alphaU.get());
		manager->serialize(stream, m_alphaV.get());
		manager->serialize(stream, m_specularReflectance.get());
		m_eta.serialize(stream);
		m_k.serialize(stream);
	}

	std::string toString() const {
		std::ostringstream oss;
		oss << "RoughConductor[" << endl
			<< "  name = \"" << getName() << "\"," << endl
			<< "  distribution = " << m_distribution.toString() << "," << endl
			<< "  alphaU = " << indent(m_alphaU->toString()) << "," << endl
			<< "  alphaV = " << indent(m_alphaV->toString()) << "," << endl
			<< "  specularReflectance = " << indent(m_specularReflectance->toString()) << "," << endl
			<< "  eta = " << m_eta.toString() << "," << endl
			<< "  k = " << m_k.toString() << endl
			<< "]";
		return oss.str();
	}

	Shader *createShader(Renderer *renderer) const;

	MTS_DECLARE_CLASS()
private:
	MicrofacetDistribution m_distribution;
	ref<Texture> m_specularReflectance;
	ref<Texture> m_alphaU, m_alphaV;
	Spectrum m_eta, m_k;
};

/**
 * GLSL port of the rough conductor shader. This version is much more
 * approximate -- it only supports the Ashikhmin-Shirley distribution, 
 * does everything in RGB, and it uses the Schlick approximation to the
 * Fresnel reflectance.
 */
class RoughConductorShader : public Shader {
public:
	RoughConductorShader(Renderer *renderer, const Texture *specularReflectance,
			const Texture *alphaU, const Texture *alphaV, const Spectrum &eta,
			const Spectrum &k) : Shader(renderer, EBSDFShader), 
			m_specularReflectance(specularReflectance), m_alphaU(alphaU), m_alphaV(alphaV){
		m_specularReflectanceShader = renderer->registerShaderForResource(m_specularReflectance.get());
		m_alphaUShader = renderer->registerShaderForResource(m_alphaU.get());
		m_alphaVShader = renderer->registerShaderForResource(m_alphaV.get());

		/* Compute the reflectance at perpendicular incidence */
		m_R0 = fresnelConductor(1.0f, eta, k);
	}

	bool isComplete() const {
		return m_specularReflectanceShader.get() != NULL &&
			   m_alphaUShader.get() != NULL &&
			   m_alphaVShader.get() != NULL;
	}

	void putDependencies(std::vector<Shader *> &deps) {
		deps.push_back(m_specularReflectanceShader.get());
		deps.push_back(m_alphaUShader.get());
		deps.push_back(m_alphaVShader.get());
	}

	void cleanup(Renderer *renderer) {
		renderer->unregisterShaderForResource(m_specularReflectance.get());
		renderer->unregisterShaderForResource(m_alphaU.get());
		renderer->unregisterShaderForResource(m_alphaV.get());
	}

	void resolve(const GPUProgram *program, const std::string &evalName, std::vector<int> &parameterIDs) const {
		parameterIDs.push_back(program->getParameterID(evalName + "_R0", false));
	}

	void bind(GPUProgram *program, const std::vector<int> &parameterIDs, int &textureUnitOffset) const {
		program->setParameter(parameterIDs[0], m_R0);
	}

	void generateCode(std::ostringstream &oss,
			const std::string &evalName,
			const std::vector<std::string> &depNames) const {
		oss << "uniform vec3 " << evalName << "_R0;" << endl
			<< endl
			<< "float " << evalName << "_D(vec3 m, float alphaU, float alphaV) {" << endl
			<< "    float ct = cosTheta(m), ds = 1-ct*ct;" << endl
			<< "    if (ds <= 0.0)" << endl
			<< "        return 0.0f;" << endl
			<< "    alphaU = 2 / (alphaU * alphaU) - 2;" << endl
			<< "    alphaV = 2 / (alphaV * alphaV) - 2;" << endl
			<< "    float exponent = (alphaU*m.x*m.x + alphaV*m.y*m.y)/ds;" << endl
			<< "    return sqrt((alphaU+2) * (alphaV+2)) * 0.15915 * pow(ct, exponent);" << endl
			<< "}" << endl
			<< endl
			<< "float " << evalName << "_G(vec3 m, vec3 wi, vec3 wo) {" << endl
			<< "    if ((dot(wi, m) * cosTheta(wi)) <= 0 || " << endl
			<< "        (dot(wo, m) * cosTheta(wo)) <= 0)" << endl
			<< "        return 0.0;" << endl
			<< "    float nDotM = cosTheta(m);" << endl
			<< "    return min(1.0, min(" << endl
			<< "        abs(2 * nDotM * cosTheta(wo) / dot(wo, m))," << endl
			<< "        abs(2 * nDotM * cosTheta(wi) / dot(wi, m))));" << endl
			<< "}" << endl
			<< endl
			<< "vec3 " << evalName << "_schlick(vec3 wi) {" << endl
			<< "    float ct = cosTheta(wi), ctSqr = ct*ct," << endl
			<< "          ct5 = ctSqr*ctSqr*ct;" << endl
			<< "    return " << evalName << "_R0 + (vec3(1.0) - " << evalName << "_R0) * ct5;" << endl
			<< "}" << endl
			<< endl
			<< "vec3 " << evalName << "(vec2 uv, vec3 wi, vec3 wo) {" << endl
			<< "   if (cosTheta(wi) <= 0 || cosTheta(wo) <= 0)" << endl
			<< "    	return vec3(0.0);" << endl
			<< "   vec3 H = normalize(wi + wo);" << endl
			<< "   vec3 reflectance = " << depNames[0] << "(uv);" << endl
			<< "   float alphaU = " << depNames[1] << "(uv).r;" << endl
			<< "   float alphaV = " << depNames[2] << "(uv).r;" << endl
			<< "   float D = " << evalName << "_D(H, alphaU, alphaV)" << ";" << endl
			<< "   float G = " << evalName << "_G(H, wi, wo);" << endl
			<< "   vec3 Fr = " << evalName << "_schlick(wi);" << endl
			<< "   return reflectance * Fr * (D * G / (4*cosTheta(wi)));" << endl
			<< "}" << endl
			<< endl
			<< "vec3 " << evalName << "_diffuse(vec2 uv, vec3 wi, vec3 wo) {" << endl
			<< "    return " << evalName << "_R0 * 0.31831 * cosTheta(wo);"<< endl
			<< "}" << endl;
	}
	MTS_DECLARE_CLASS()
private:
	ref<const Texture> m_specularReflectance;
	ref<const Texture> m_alphaU;
	ref<const Texture> m_alphaV;
	ref<Shader> m_specularReflectanceShader;
	ref<Shader> m_alphaUShader;
	ref<Shader> m_alphaVShader;
	Spectrum m_R0;
};

Shader *RoughConductor::createShader(Renderer *renderer) const { 
	return new RoughConductorShader(renderer,
		m_specularReflectance.get(), m_alphaU.get(), m_alphaV.get(), m_eta, m_k);
}

MTS_IMPLEMENT_CLASS(RoughConductorShader, false, Shader)
MTS_IMPLEMENT_CLASS_S(RoughConductor, false, BSDF)
MTS_EXPORT_PLUGIN(RoughConductor, "Rough conductor BRDF");
MTS_NAMESPACE_END
