/*******************************************************************************
 * Copyright (c) 2023 Alexander Couzens.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Public License v3.0
 * which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/gpl.html
 * 
 * Contributors:
 *     Alexander Couzens - copy based on UHD
 ******************************************************************************/
package martin.tempest.sources;

/**
 * SoapyUDH driver
 * 
 * @author Alexander Couzens
 *
 */
public class TSDRSoapySource extends TSDRSource {

	public TSDRSoapySource() {
		super("Soapy", "TSDRPlugin_Soapy", false);
	}

}
